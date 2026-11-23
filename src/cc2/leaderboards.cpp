// A worker thread caches the server's rows per view and the game thread completes the parked enumerations from it.

#include "cc2/leaderboards.h"
#include "cc2/clips.h"
#include "cc2/kernel/xam/local_players.h"
#include "cc2/kernel/xnet.h"
#include "cc2/lobby.h"
#include "cc2/online.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xio.h>
#include <rex/types.h>

// The runtime's XamEnumerate (exported from the runtime DLL).
namespace rex::kernel::xam {
u32 XamEnumerate_entry(u32 handle, u32 flags, mapped_void buffer, u32 buffer_length, mapped_u32 items_returned,
                       ppc_ptr_t<rex::system::XAM_OVERLAPPED> overlapped);
}  // namespace rex::kernel::xam

extern "C" REX_FUNC(__imp__sub_82242680);  // returns a bool from a script command (result, bool*)
extern "C" REX_FUNC(__imp__sub_82AF3580);  // NETMANXA: sends the leaderboard queue's front request (netman, request)
extern "C" REX_FUNC(__imp__sub_82AF4E70);  // NETMANXA::Step(netman)
extern "C" REX_FUNC(__imp__sub_82AFC7C0);  // GameLeaderboards::Controller::Controller(this)
extern "C" REX_FUNC(__imp__sub_82AFCA18);  // Controller::Open(this, user, board, filter, centered)

namespace cc2::boards {
namespace {

using rex::u32;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kIoPending = 997;
constexpr uint32_t kHeaderSize = sizeof(kernel::X_USER_STATS_READ_RESULTS) + sizeof(kernel::X_USER_STATS_VIEW);
constexpr uint32_t kRowSize = sizeof(kernel::X_USER_STATS_ROW);
constexpr size_t kTagSize = 15;  // a gamertag, less its terminator
constexpr size_t kMaxRows = 100;
constexpr uint32_t kXuidHigh = 0xE0000000u;
constexpr auto kFresh = std::chrono::minutes(2);    // the server keeps its own copy for ten
constexpr auto kRetry = std::chrono::seconds(15);   // after a fetch that failed
constexpr auto kGiveUp = std::chrono::seconds(10);  // an enumeration never waits on the server longer

// The game (TU1).
constexpr uint32_t kNetman = 0x82EDAAE4;            // NETMANXA*
constexpr uint32_t kPageController = 0x26850;       // NETMANXA: the leaderboard page
constexpr uint32_t kMiniController = 0x26854;       // NETMANXA: course select's mini board
constexpr uint32_t kControllerVtable = 0x821A17C0;  // GameLeaderboards::Controller, a request's listener
constexpr uint32_t kFilter = 0x1C, kPosition = 0x2C;  // Controller: its filter; the row its view opens on
constexpr uint32_t kEveryone = 0, kFriends = 1, kMyScore = 2;
constexpr uint32_t kNoByte = 0x82167234;  // a zero byte: the end of "lb/total_fmt"

// Wraps the scripts' SetButtonText(button, text id, context) to blank the page's Y prompt id, which hides the prompt.
constexpr char kPromptScript[] = R"lua(
if type(SetButtonText)~='function' then return 0 end
local set=SetButtonText
SetButtonText=function(button,text,context)
  if text=='common/ui/sys/but/change_timeframe' then text='' end
  return set(button,text,context)
end
return 1
)lua";

struct Row {
  std::string name;
  uint32_t time = 0;  // milliseconds
};

struct Board {
  bool tried = false;  // a fetch has finished
  bool ok = false;     // and rows are the server's
  bool queued = false;
  Clock::time_point at;  // when the last fetch finished
  std::vector<Row> rows;
};

struct Query {
  uint32_t view = 0;
  uint32_t start = 1;  // the first rank wanted
  uint32_t rows = 0;
};

struct Parked {
  uint32_t buffer = 0, size = 0;
  Query query;
  Clock::time_point since;
};

std::mutex g_mutex;  // guards everything below
std::condition_variable g_wake;
std::map<uint32_t, Board> g_boards;    // by view
std::deque<uint32_t> g_queue;          // views to fetch
std::map<uint32_t, Query> g_queries;   // by enumerator handle, until it is enumerated
std::map<uint32_t, Parked> g_parked;   // by overlapped
bool g_stop = false;
std::thread g_thread;
thread_local bool t_screen = false;  // inside a request that the page or the mini board queued

template <typename T>
T* Guest(uint32_t address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<T*>(address);
}
uint32_t Load(const uint8_t* base, uint32_t address) {
  return *reinterpret_cast<const rex::be<uint32_t>*>(base + address);
}
void Store(uint8_t* base, uint32_t address, uint32_t value) {
  *reinterpret_cast<rex::be<uint32_t>*>(base + address) = value;
}

// A course's all-time view: 0-5, 12-17, 24-29, 36-41, 48-53 and the odd ids 61-69, not the weekly views or 60 (gold).
bool AllTime(uint32_t view) { return view < 60 ? view % 12 < 6 : view <= 69 && view % 2 == 1; }

// ---- The server's rows -----------------------------------------------------------

// Enough JSON for the reply: {"rows": [{"rank": 1, "name": "...", "time": 37883}, ...], ...}.
struct Reader {
  const std::string& s;
  size_t at = 0;

  void Space() {
    while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n')) ++at;
  }
  bool Take(char c) {
    Space();
    if (at >= s.size() || s[at] != c) return false;
    ++at;
    return true;
  }
  // A string in the characters a gamertag has; any other character becomes '?'.
  bool String(std::string* out) {
    if (!Take('"')) return false;
    out->clear();
    while (at < s.size() && s[at] != '"') {
      uint32_t c = uint8_t(s[at++]);
      if (c == '\\' && at < s.size()) {
        c = uint8_t(s[at++]);
        if (c == 'u') {
          c = uint32_t(std::strtoul(s.substr(at, 4).c_str(), nullptr, 16));
          at = std::min(at + 4, s.size());
          if (c >= 0xDC00 && c < 0xE000) continue;  // the second half of a pair
        } else if (c != '"' && c != '\\' && c != '/') {
          c = ' ';
        }
      } else if ((c & 0xC0) == 0x80) {
        continue;  // inside a UTF-8 character
      }
      *out += c >= 0x20 && c < 0x7F ? char(c) : '?';
    }
    return Take('"');
  }
  // Steps over any value.
  bool Skip() {
    std::string ignored;
    Space();
    if (at >= s.size()) return false;
    if (s[at] == '"') return String(&ignored);
    if (s[at] == '{' || s[at] == '[') {
      const char close = s[at++] == '{' ? '}' : ']';
      while (!Take(close)) {
        if (close == '}' && !(String(&ignored) && Take(':'))) return false;
        if (!Skip()) return false;
        Take(',');
      }
      return true;
    }
    const size_t from = at;
    while (at < s.size() && !std::strchr(",}] \t\r\n", s[at])) ++at;
    return at > from;
  }
  // Steps over a number or literal and returns its text.
  bool Plain(std::string* out) {
    Space();
    const size_t from = at;
    if (at < s.size() && (s[at] == '"' || s[at] == '{' || s[at] == '[')) return false;
    if (!Skip()) return false;
    out->assign(s, from, at - from);
    return true;
  }
};

// The rows of a reply; false when it has no board ("unavailable", or not the reply at all).
bool ParseRows(const std::string& json, std::vector<Row>* rows) {
  Reader in{json};
  bool found = false, unavailable = false;
  std::string key, value;
  if (!in.Take('{')) return false;
  while (!in.Take('}')) {
    if (!in.String(&key) || !in.Take(':')) return false;
    if (key == "rows") {
      if (!in.Take('[')) return false;
      while (!in.Take(']')) {
        Row row;
        long long time = 0;
        if (!in.Take('{')) return false;
        while (!in.Take('}')) {
          if (!in.String(&key) || !in.Take(':')) return false;
          if (key == "name") {
            if (!in.String(&row.name)) return false;
          } else if (key == "time") {
            if (!in.Plain(&value)) return false;
            time = std::atoll(value.c_str());
          } else if (!in.Skip()) {
            return false;
          }
          in.Take(',');
        }
        if (!row.name.empty() && time > 0 && time < 0x7FFFFFFF && rows->size() < kMaxRows) {
          row.time = uint32_t(time);
          rows->push_back(std::move(row));
        }
        in.Take(',');
      }
      found = true;
    } else if (key == "unavailable") {
      if (!in.Plain(&value)) return false;
      unavailable = value == "true";
    } else if (!in.Skip()) {
      return false;
    }
    in.Take(',');
  }
  return found && !unavailable;
}

// Asks the worker for a board that is missing or old, ahead of the others; g_mutex held.
void Want(uint32_t view) {
  Board& board = g_boards[view];
  if (board.queued || (board.tried && Clock::now() - board.at < (board.ok ? kFresh : kRetry))) return;
  board.queued = true;
  g_queue.push_front(view);
  g_wake.notify_all();
}

// The board can be shown: it has the server's rows (a newer copy may be on its way), or the fetch for them failed.
bool Ready(const Board& board) { return board.ok || (board.tried && !board.queued); }

void Worker() {
  std::unique_lock lock(g_mutex);
  while (!g_stop) {
    if (g_queue.empty()) {
      g_wake.wait(lock);
      continue;
    }
    const uint32_t view = g_queue.front();
    g_queue.pop_front();
    lock.unlock();
    std::string body;
    std::vector<Row> rows;
    const int status = online::ServerRequest("GET", "/v1/speedrun/" + std::to_string(view), "", body);
    const bool ok = status == 200 && ParseRows(body, &rows);
    if (!ok) rows.clear();
    // -1: no account is signed in; 0: the server can't be reached.
    REXLOG_INFO("[boards] view {}: {} rows (server {})", view, rows.size(), status);
    lock.lock();
    Board& board = g_boards[view];
    board.tried = true;
    board.ok = ok;
    board.queued = false;
    board.at = Clock::now();
    board.rows = std::move(rows);  // a failed fetch leaves an empty board
  }
}

// ---- Results in the game's format ------------------------------------------------

// A row's XUID hashed from the name, its low half never a local player's or the game takes the row for that player's.
uint64_t RowXuid(const std::string& name, const uint32_t* local) {
  uint32_t low = 0x811C9DC5u;
  for (const unsigned char c : name) low = (low ^ (c >= 'A' && c <= 'Z' ? c + 32u : c)) * 0x01000193u;
  while (std::find(local, local + kernel::xam::kLocalXuids, low) != local + kernel::xam::kLocalXuids) ++low;
  return uint64_t(kXuidHigh) << 32 | low;
}

// Writes XUSER_STATS_READ_RESULTS with one view, the rows the query covers and no columns.
void Fill(uint32_t buffer, uint32_t size, const Query& query, const std::vector<Row>& rows) {
  uint32_t local[kernel::xam::kLocalXuids];
  kernel::xam::LocalXuidLows(local);
  uint8_t* out = Guest<uint8_t>(buffer);
  std::memset(out, 0, size);
  const uint32_t total = uint32_t(rows.size());
  const uint32_t first = std::min(query.start - 1, total);
  const uint32_t count = std::min({total - first, query.rows, (size - kHeaderSize) / kRowSize});
  auto* results = reinterpret_cast<kernel::X_USER_STATS_READ_RESULTS*>(out);
  auto* view = reinterpret_cast<kernel::X_USER_STATS_VIEW*>(results + 1);
  auto* row = reinterpret_cast<kernel::X_USER_STATS_ROW*>(view + 1);
  results->num_views = 1;
  results->views_ptr = buffer + uint32_t(sizeof(*results));
  view->view_id = query.view;
  view->total_view_rows = count ? total : 0;  // a total without rows makes the game ask again from the top
  view->num_rows = count;
  view->rows_ptr = buffer + kHeaderSize;
  for (uint32_t i = 0; i < count; ++i, ++row) {
    const Row& from = rows[first + i];
    row->xuid = RowXuid(from.name, local);
    row->rank = first + i + 1;  // gapless from 1: the game keeps a row at position rank - 1
    row->i64Rating = uint64_t(-int64_t(from.time));  // the time, negated
    std::memcpy(row->gamertag, from.name.data(), std::min(from.name.size(), kTagSize));
  }
}

// ---- The enumerations ------------------------------------------------------------

// XamEnumerate(handle, flags, buffer, size, &items, overlapped): a tracked handle's request waits for its board.
u32 Enumerate(u32 handle, u32 flags, mapped_void buffer, u32 buffer_length, mapped_u32 items_returned,
              ppc_ptr_t<rex::system::XAM_OVERLAPPED> overlapped) {
  // The Time Trial clip list's request waits for the folder scan.
  if (buffer && overlapped &&
      clips::Park(handle, buffer.guest_address(), buffer_length, overlapped.guest_address())) {
    overlapped->result = kIoPending;
    overlapped->length = 0;
    return kIoPending;
  }
  std::unique_lock lock(g_mutex);
  const auto it = g_queries.find(handle);
  if (it == g_queries.end() || !buffer || buffer_length < kHeaderSize) {
    lock.unlock();
    return rex::kernel::xam::XamEnumerate_entry(handle, flags, buffer, buffer_length, items_returned, overlapped);
  }
  const Query query = it->second;
  g_queries.erase(it);
  if (!overlapped) {
    Fill(buffer.guest_address(), buffer_length, query, g_boards[query.view].rows);
    if (items_returned) *items_returned = 1;
    return 0;
  }
  Want(query.view);
  g_parked[overlapped.guest_address()] = {buffer.guest_address(), buffer_length, query, Clock::now()};
  overlapped->result = kIoPending;
  overlapped->length = 0;
  return kIoPending;
}

// Completes the waiting requests whose boards are in; game thread.
void Complete() {
  std::lock_guard lock(g_mutex);
  for (auto it = g_parked.begin(); it != g_parked.end();) {
    const Parked& parked = it->second;
    const Board& board = g_boards[parked.query.view];
    if (!Ready(board) && Clock::now() - parked.since < kGiveUp) {
      ++it;
      continue;
    }
    Fill(parked.buffer, parked.size, parked.query, board.rows);
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(it->first, 0, 0, 0);
    it = g_parked.erase(it);
  }
}

// Marks only the page's and mini board's requests: the Time Trial clip list would download a clip for every row.
void Send(PPCContext& ctx, uint8_t* base) {
  const uint32_t listener = Load(base, ctx.r4.u32);
  t_screen = listener && Load(base, listener) == kControllerVtable;
  clips::Sending(base, ctx.r4.u32);
  __imp__sub_82AF3580(ctx, base);
  t_screen = false;
  clips::Sent();
}

// Controller::Open, where a controller takes its filter and shows the filter's name.
void Open(PPCContext& ctx, uint8_t* base) {
  const uint32_t netman = Load(base, kNetman), controller = ctx.r3.u32;
  if (netman && controller == Load(base, netman + kPageController)) {
    if (ctx.r6.u32 == kFriends) ctx.r6.u64 = kMyScore;  // the page has no friends filter
  } else if (netman && controller == Load(base, netman + kMiniController)) {
    // The mini board asks for friends centered on the player and gets the top of the board with no row marked.
    ctx.r6.u64 = kEveryone;
    ctx.r7.u64 = 0;
    Store(base, controller + kPosition, 0xFFFFFFFFu);
  }
  __imp__sub_82AFCA18(ctx, base);
}

}  // namespace

void Start() { g_thread = std::thread(Worker); }

void Tick(PPCContext& ctx, uint8_t* base) {
  static uint32_t installed = 0;  // the Lua state that has kPromptScript
  static int wait = 0;
  const uint32_t state = lobby::MainLuaState();
  if (!state || state == installed || wait-- > 0) return;
  if (lobby::RunLua(ctx, base, state, kPromptScript) == 1) {
    installed = state;
  } else {
    wait = 30;  // the state does not have the game's scripts yet
  }
}

void Stop() {
  {
    std::lock_guard lock(g_mutex);
    g_stop = true;
  }
  g_wake.notify_all();
  if (g_thread.joinable()) g_thread.join();
}

uint32_t Track(uint32_t handle, uint32_t kind, uint64_t pivot, uint32_t rows, uint32_t specs, uint32_t view) {
  if (!t_screen || kind != kernel::X_STATS_ENUMERATOR_TYPE::RANK || specs != 1 || !AllTime(view)) return 0;
  std::lock_guard lock(g_mutex);
  if (g_queries.size() >= 64) g_queries.erase(g_queries.begin());  // one never enumerated
  g_queries[handle] = {view, std::max<uint32_t>(uint32_t(pivot), 1), rows};
  Want(view);
  return kHeaderSize + rows * kRowSize;
}

void Cancel(uint32_t overlapped) {
  std::lock_guard lock(g_mutex);
  g_parked.erase(overlapped);
}

}  // namespace cc2::boards

REX_EXPORT(__imp__XamEnumerate, cc2::boards::Enumerate)

REX_HOOK_RAW(sub_82AF3580) { cc2::boards::Send(ctx, base); }

// The step polls the overlapped of the request in flight.
REX_HOOK_RAW(sub_82AF4E70) {
  cc2::boards::Complete();
  cc2::clips::Complete();
  __imp__sub_82AF4E70(ctx, base);
}

// The page opens on its controller's filter, which starts as friends.
REX_HOOK_RAW(sub_82AFC7C0) {
  const uint32_t controller = ctx.r3.u32;
  __imp__sub_82AFC7C0(ctx, base);
  cc2::boards::Store(base, controller + cc2::boards::kFilter, cc2::boards::kEveryone);
}

REX_HOOK_RAW(sub_82AFCA18) { cc2::boards::Open(ctx, base); }

// Controller::ToggleType(this), the page's Y: a no-op, as the weekly boards it alone leads to have no rows.
REX_HOOK_RAW(sub_82AFCB20) { (void)base; }

// LbCanChallengeGameclip(result, netman): the served rows have no game clips, so the answer stays the empty board's.
REX_HOOK_RAW(sub_82AF6558) {
  ctx.r4.u64 = cc2::boards::kNoByte;
  __imp__sub_82242680(ctx, base);
}
