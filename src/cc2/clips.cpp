// A worker thread writes the run's clip and scans the folder; the game thread answers the Time Trial list from it.

#include "cc2/clips.h"
#include "cc2/avatar_fetch.h"
#include "cc2/clip_file.h"
#include "cc2/kernel/xam/local_players.h"
#include "cc2/kernel/xnet.h"
#include "cc2/lobby.h"
#include "cc2/online.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/lzx.h>
#include <rex/types.h>

REXCVAR_DECLARE(std::string, user_name);
REXCVAR_DECLARE(bool, online_mode);

extern "C" REX_FUNC(__imp__sub_82242680);  // returns a bool from a script command (result, bool*)
extern "C" REX_FUNC(__imp__sub_82AE32A8);  // NEMESISLAUNCHER::Launch(this)
extern "C" REX_FUNC(__imp__sub_82B04DC8);  // GameClipRecorder::GetCompressedClip(time, flag, {data, size}*, avatar)
extern "C" REX_FUNC(__imp__sub_82B58F68);  // LeaderboardMANAGER: a level's boards (level, boards[2])
extern "C" REX_FUNC(__imp__sub_82B714B8);  // GAMEXA::PostToLeaderboard(this)
extern "C" REX_FUNC(__imp__sub_82B73870);  // GAMEXA::TallyShowGamercard(result, this)

namespace cc2::clips {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kResultsSize = sizeof(kernel::X_USER_STATS_READ_RESULTS) + sizeof(kernel::X_USER_STATS_VIEW);
constexpr uint32_t kRowSize = sizeof(kernel::X_USER_STATS_ROW);
constexpr size_t kMaxRows = 50;     // the list's one request
constexpr size_t kMaxFiles = 2000;  // folder entries looked at per scan
constexpr size_t kMaxAvatars = 16;  // manifests remembered as clip ghosts
constexpr uint32_t kXuidHigh = 0xE0000000u;
constexpr uint32_t kUnranked = 0x80000000u;  // ranks below 1 as the game reads them: the results screen shows "--"
constexpr auto kGiveUp = std::chrono::seconds(5);  // a request never waits on the scan longer

// The game (TU1).
constexpr uint32_t kListVtable = 0x821A4500;     // NemesisLauncher's LeaderboardQuery, a request's listener
constexpr uint32_t kRecorder = 0x82DA0A24;       // GameClipRecorder: the recorded player, 0 between races
constexpr uint32_t kRecorderStart = 0x82DA0A40;  // GameClipRecorder: when its recording began
constexpr uint32_t kNoByte = 0x82167234;         // a zero byte
constexpr uint32_t kView = 4, kCount = 12, kMode = 16;  // a queued request's fields
constexpr uint32_t kByRank = 0;
constexpr uint32_t kClipsBegin = 80, kClipsEnd = 84, kClipSize = 568;  // NemesisLauncher: the clips added
constexpr uint32_t kLevel = 76, kLevelKey = 128, kBoardView = 0x54;    // GAMEXA, its level, a LeaderboardMASTER

// Sets the results screen's left stick prompt, named text_ls or text_l3 by game mode, to the text appended.
constexpr char kPromptHead[] = R"lua(
local function label(name)
  local ok,l=pcall(function()
    local c=gui.main.tally.buttons[name]
    return c and type(c.GetAbsScreenPos)=='function' and c or nil
  end)
  return ok and l or nil
end
local l=label('text_ls') or label('text_l3')
if not l then return 0 end
return pcall(function() l.Text=)lua";
constexpr char kPromptTail[] = " end) and 1 or 0";
constexpr char kSaveText[] = "ToUniString('Save Clip')";
constexpr char kSavedText[] = "ToUniString('Game clip saved')";

enum class Save { kNone, kWanted, kDone };

// The run on the results screen.
struct Run {
  uint32_t id = 0;
  std::vector<uint8_t> blob;       // what GetCompressedClip made for the upload
  uint64_t key = 0;                // its first hash bytes
  int course = -1;
  uint32_t player = 0, start = 0;  // the recorder's state then: a new race changes it
  Save save = Save::kNone;
};

struct Job {
  uint32_t run = 0;
  uint64_t key = 0;
  int course = -1;
  std::string player;
  std::vector<uint8_t> blob;
};

struct Entry {
  Clip clip;
  uint64_t xuid = 0;
  std::string blob;  // the file less its trailer
};

struct Query {
  int course = -1;
  uint32_t view = 0, scan = 0;
};

struct Parked {
  uint32_t buffer = 0, size = 0;
  Query query;
  Clock::time_point since;
};

std::mutex g_mutex;  // guards everything below
std::condition_variable g_wake;
Run g_run;
std::optional<Job> g_job;
uint64_t g_saved = 0;  // the last clip written, which is not written again
std::vector<Entry> g_list;  // the scanned course's clips, slowest first
int g_list_course = -1, g_scan_course = -1;
std::deque<std::array<uint8_t, kManifestSize>> g_avatars;  // of the clips handed to the game, newest last
uint32_t g_scans_wanted = 0, g_scans_done = 0;
uint32_t g_local[kernel::xam::kLocalXuids] = {};
std::map<uint32_t, Query> g_queries;  // by enumerator handle, until it is enumerated
std::map<uint32_t, Parked> g_parked;  // by overlapped
bool g_stop = false;
std::thread g_thread;
std::atomic<bool> g_racing{false};
thread_local bool t_posting = false;     // inside PostToLeaderboard
thread_local std::vector<uint8_t> t_blob;
thread_local Query t_list;               // inside the list's request; scan 0 otherwise

uint32_t Load(const uint8_t* base, uint32_t address) {
  return *reinterpret_cast<const rex::be<uint32_t>*>(base + address);
}
void Store(uint8_t* base, uint32_t address, uint32_t value) {
  *reinterpret_cast<rex::be<uint32_t>*>(base + address) = value;
}

std::filesystem::path Folder() { return rex::filesystem::GetExecutableFolder() / "clips"; }

// A file name in ASCII; any other character becomes '?', which no saved name has.
std::string Name(const std::filesystem::path& path) {
  std::string name;
  for (const auto c : path.filename().native()) name += c > 0x20 && c < 0x7F ? char(c) : '?';
  return name;
}

bool IsClip(const std::string& name) {
  const size_t size = sizeof(kExtension) - 1;
  if (name.size() <= size) return false;
  for (size_t i = 0; i < size; ++i) {
    if ((name[name.size() - size + i] | 0x20) != kExtension[i]) return false;
  }
  return true;
}

std::string TimeText(uint32_t ms) {
  char text[16];
  std::snprintf(text, sizeof(text), "%02u:%02u.%03u", ms / 60000, ms / 1000 % 60, ms % 1000);
  return text;
}

// ---- The worker --------------------------------------------------------------------

// Writes the job's clip as <player>_<course>_<date>_<n>.clip through a temporary name.
bool Write(const Job& job) {
  uint32_t time = 0;
  if (!Native(job.blob.data(), job.blob.size(), &lzx_decompress, &time)) {
    REXLOG_WARN("[clips] the run's clip is not one the game would load; nothing saved");
    return false;
  }
  const std::vector<uint8_t> file = Pack(job.blob.data(), job.blob.size(), job.course, job.player);
  Clip check;
  if (!Parse(file.data(), file.size(), &lzx_decompress, &check)) return false;
  const std::filesystem::path folder = Folder();
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  std::vector<std::string> names;
  for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
    names.push_back(Name(it->path()));
  }
  if (error) {
    REXLOG_WARN("[clips] could not read {}: {}", Name(folder), error.message());
    return false;
  }
  char date[16] = "";
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  std::strftime(date, sizeof(date), "%Y-%m-%d", &local);
  const std::string stem = Stem(job.player, job.course, date);
  const std::string name = stem + std::to_string(NextNumber(names, stem)) + kExtension;
  const std::filesystem::path temporary = folder / (name + ".tmp");
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    out.flush();
    if (!out) error = std::make_error_code(std::errc::io_error);
  }
  if (!error) std::filesystem::rename(temporary, folder / name, error);
  if (error) {
    REXLOG_WARN("[clips] could not write {}: {}", name, error.message());
    std::filesystem::remove(temporary, error);
    return false;
  }
  REXLOG_INFO("[clips] saved {} ({} bytes, {} by {})", name, file.size(), TimeText(time), check.name);
  return true;
}

// The course's valid clips in the folder: the fastest kMaxRows, one per recording, slowest first.
std::vector<Entry> Scan(int course, const uint32_t* local) {
  std::vector<Entry> list;
  std::error_code error;
  size_t seen = 0, skipped = 0;
  for (std::filesystem::directory_iterator it(Folder(), error), end; !error && it != end && seen < kMaxFiles;
       it.increment(error), ++seen) {
    std::error_code ignored;
    if (!IsClip(Name(it->path())) || !it->is_regular_file(ignored)) continue;
    const uintmax_t size = it->file_size(ignored);
    if (ignored || size < kHeaderSize + kTrailerSize || size > kMaxFileSize) {
      ++skipped;
      continue;
    }
    // The trailer first: other courses' files cost one small read.
    std::ifstream in(it->path(), std::ios::binary);
    uint8_t trailer[kTrailerSize];
    in.seekg(std::streamoff(size - kTrailerSize));
    in.read(reinterpret_cast<char*>(trailer), sizeof(trailer));
    if (!in || std::memcmp(trailer, kTrailerMagic, 8)) {
      ++skipped;
      continue;
    }
    if (CourseOfLevel(Be32(trailer + 8)) != course) continue;
    Entry entry;
    entry.blob.resize(size_t(size));
    in.seekg(0);
    in.read(entry.blob.data(), std::streamsize(entry.blob.size()));
    const auto* bytes = reinterpret_cast<const uint8_t*>(entry.blob.data());
    if (!in || !Parse(bytes, entry.blob.size(), &lzx_decompress, &entry.clip)) {
      ++skipped;
      continue;
    }
    entry.blob.resize(entry.clip.native_size);
    list.push_back(std::move(entry));
  }
  std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
    return a.clip.time != b.clip.time ? a.clip.time < b.clip.time : a.clip.key < b.clip.key;
  });
  list.erase(std::unique(list.begin(), list.end(),
                         [](const Entry& a, const Entry& b) { return a.clip.key == b.clip.key; }),
             list.end());
  if (list.size() > kMaxRows) list.resize(kMaxRows);
  // The game tells rows apart by the low half of the XUID and takes a local player's for that player.
  std::vector<uint32_t> used(local, local + kernel::xam::kLocalXuids);
  for (Entry& entry : list) {
    uint32_t low = uint32_t(entry.clip.key);
    while (!low || std::find(used.begin(), used.end(), low) != used.end()) ++low;
    used.push_back(low);
    entry.xuid = uint64_t(kXuidHigh) << 32 | low;
  }
  std::reverse(list.begin(), list.end());  // the game adds from the last row back
  // Items this install lacks start downloading now, well before a ghost wearing them is built.
  for (const Entry& entry : list) {
    if (REXCVAR_GET(online_mode)) avatar_fetch::QueueManifest(entry.clip.avatar.data());
  }
  REXLOG_INFO("[clips] {}: {} clips ({} files skipped)", kCourseNames[course], list.size(), skipped);
  return list;
}

void Worker() {
  std::unique_lock lock(g_mutex);
  while (!g_stop) {
    if (g_job) {
      const Job job = std::move(*g_job);
      g_job.reset();
      lock.unlock();
      bool saved = false;
      try {
        saved = Write(job);
      } catch (const std::exception& e) {
        REXLOG_WARN("[clips] save failed: {}", e.what());
      }
      lock.lock();
      if (saved) g_saved = job.key;
      if (g_run.id == job.run) g_run.save = saved ? Save::kDone : Save::kNone;
    } else if (g_scans_done != g_scans_wanted) {
      const uint32_t scan = g_scans_wanted;
      const int course = g_scan_course;
      uint32_t local[kernel::xam::kLocalXuids];
      std::memcpy(local, g_local, sizeof(local));
      lock.unlock();
      std::vector<Entry> list;
      try {
        list = Scan(course, local);
      } catch (const std::exception& e) {
        REXLOG_WARN("[clips] scan failed: {}", e.what());
      }
      lock.lock();
      g_list = std::move(list);
      g_list_course = course;
      g_scans_done = scan;
    } else {
      g_wake.wait(lock);
    }
  }
}

// ---- The results screen ------------------------------------------------------------

// The recorder still holds the run the clip was made from; g_mutex held.
bool Current(const uint8_t* base) {
  return !g_run.blob.empty() && Load(base, kRecorder) == g_run.player && Load(base, kRecorderStart) == g_run.start;
}

// GetCompressedClip returned inside PostToLeaderboard: keeps the blob it made when the loader would take it back.
void Compressed(const uint8_t* base, uint32_t out) {
  if (!t_posting || !out) return;
  const uint32_t data = Load(base, out), size = Load(base, out + 4);
  uint32_t time = 0;
  if (!data || size > kMaxFileSize || !Native(base + data, size, &lzx_decompress, &time)) return;
  t_blob.assign(base + data, base + data + size);
}

// PostToLeaderboard runs as the results screen opens and compresses the clip of a run it would post.
void Post(PPCContext& ctx, uint8_t* base) {
  const uint32_t game = ctx.r3.u32;
  {
    std::lock_guard lock(g_mutex);
    g_run.blob.clear();
    g_run.save = Save::kNone;
  }
  t_blob.clear();
  t_posting = true;
  __imp__sub_82B714B8(ctx, base);
  t_posting = false;
  if (t_blob.empty()) return;
  // The course is the one whose all-time board the level posts to.
  const PPCContext saved = ctx;
  const uint32_t boards = (ctx.r1.u32 - 0x200) & ~15u;
  Store(base, boards, 0);
  ctx.r1.u32 = boards - 0x100;
  ctx.r3.u64 = Load(base, Load(base, game + kLevel) + kLevelKey);
  ctx.r4.u64 = boards;
  __imp__sub_82B58F68(ctx, base);
  const uint32_t board = ctx.r3.u32 ? Load(base, boards) : 0;
  ctx = saved;
  const int course = board ? CourseOfView(Load(base, board + kBoardView)) : -1;
  if (course < 0) {
    REXLOG_WARN("[clips] the run's course has no known board; its clip can't be saved");
    return;
  }
  std::lock_guard lock(g_mutex);
  ++g_run.id;
  g_run.blob = std::move(t_blob);
  g_run.key = uint64_t(Be32(g_run.blob.data() + 16)) << 32 | Be32(g_run.blob.data() + 20);
  g_run.save = g_run.key == g_saved ? Save::kDone : Save::kNone;
  g_run.course = course;
  g_run.player = Load(base, kRecorder);
  g_run.start = Load(base, kRecorderStart);
  REXLOG_INFO("[clips] {}: a {} byte clip can be saved", kCourseNames[course], g_run.blob.size());
}

// The results screen's left stick command: saves the run's clip once, or shows the gamer card when there is none.
void Press(PPCContext& ctx, uint8_t* base) {
  std::string player = online::SignedInName();
  if (player.empty()) player = REXCVAR_GET(user_name);
  {
    std::unique_lock lock(g_mutex);
    if (!Current(base)) {
      lock.unlock();
      __imp__sub_82B73870(ctx, base);
      return;
    }
    if (g_run.save == Save::kNone) {
      g_run.save = Save::kWanted;
      g_job = Job{g_run.id, g_run.key, g_run.course, std::move(player), g_run.blob};
      g_wake.notify_all();
    }
  }
  const uint32_t result = ctx.r3.u32;
  ctx.r4.u64 = kNoByte;
  __imp__sub_82242680(ctx, base);
  ctx.r3.u64 = result;
}

// ---- The Time Trial list -----------------------------------------------------------

// Writes XUSER_STATS_READ_RESULTS with one view and the course's clips as rows; g_mutex held.
void Fill(uint32_t buffer, uint32_t size, const Query& query) {
  uint8_t* out = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(buffer);
  std::memset(out, 0, size);
  const bool scanned = g_scans_done - query.scan < 0x80000000u && g_list_course == query.course;
  const uint32_t count = scanned ? uint32_t(std::min<size_t>(g_list.size(), (size - kResultsSize) / kRowSize)) : 0;
  auto* results = reinterpret_cast<kernel::X_USER_STATS_READ_RESULTS*>(out);
  auto* view = reinterpret_cast<kernel::X_USER_STATS_VIEW*>(results + 1);
  auto* row = reinterpret_cast<kernel::X_USER_STATS_ROW*>(view + 1);
  results->num_views = 1;
  results->views_ptr = buffer + uint32_t(sizeof(*results));
  view->view_id = query.view;
  view->total_view_rows = count;
  view->num_rows = count;
  view->rows_ptr = buffer + kResultsSize;
  // The fastest clips are the last rows, whatever the buffer holds.
  for (size_t i = g_list.size() - count; i < g_list.size(); ++i, ++row) {
    const Entry& entry = g_list[i];
    row->xuid = entry.xuid;
    row->rank = kUnranked + 1 + uint32_t(i);
    row->i64Rating = uint64_t(-int64_t(entry.clip.time));  // the time, negated
    std::memcpy(row->gamertag, entry.clip.name.data(), std::min(entry.clip.name.size(), kTagSize));
  }
}

}  // namespace

void Start() { g_thread = std::thread(Worker); }

void Stop() {
  {
    std::lock_guard lock(g_mutex);
    g_stop = true;
  }
  g_wake.notify_all();
  if (g_thread.joinable()) g_thread.join();
}

void Tick(PPCContext& ctx, uint8_t* base) {
  static uint32_t wanted = 0;  // the run and state the prompt was last tried for
  static int wait = 0, misses = 0;
  uint32_t state = 0;
  {
    std::lock_guard lock(g_mutex);
    if (!Current(base)) return;
    state = g_run.id * 2 + (g_run.save == Save::kDone);
  }
  if (state != wanted) wait = misses = 0;
  wanted = state;
  if (wait-- > 0) return;
  const uint32_t lua = lobby::MainLuaState();
  if (!lua) return;
  const std::string script = std::string(kPromptHead) + (state & 1 ? kSavedText : kSaveText) + kPromptTail;
  misses = lobby::RunLua(ctx, base, lua, script.c_str()) == 1 ? 0 : misses + 1;
  // Set again now and then, as the screen can be rebuilt; rarely once it has been gone a while.
  wait = !misses ? 30 : misses < 100 ? 5 : 300;
}

void Sending(uint8_t* base, uint32_t request) {
  // The list's first read asks for 50 friends; its later reads stay the empty boards they are.
  const uint32_t listener = Load(base, request);
  if (!listener || Load(base, listener) != kListVtable || Load(base, request + kCount) != kMaxRows) return;
  const uint32_t view = Load(base, request + kView);
  const int course = CourseOfView(view);
  if (course < 0) return;
  Store(base, request + kMode, kByRank);  // a friends read has room for one row
  std::lock_guard lock(g_mutex);
  kernel::xam::LocalXuidLows(g_local);
  g_scan_course = course;
  t_list = {course, view, ++g_scans_wanted};
  g_wake.notify_all();
}

void Sent() { t_list = {}; }

uint32_t Track(uint32_t handle, uint32_t kind, uint32_t rows, uint32_t specs, uint32_t view) {
  if (t_list.course < 0 || kind != kernel::X_STATS_ENUMERATOR_TYPE::RANK || specs != 1 || view != t_list.view) return 0;
  std::lock_guard lock(g_mutex);
  if (g_queries.size() >= 64) g_queries.erase(g_queries.begin());  // one never enumerated
  g_queries[handle] = t_list;
  return kResultsSize + rows * kRowSize;
}

bool Park(uint32_t handle, uint32_t buffer, uint32_t size, uint32_t overlapped) {
  std::lock_guard lock(g_mutex);
  const auto it = g_queries.find(handle);
  if (it == g_queries.end() || size < kResultsSize) return false;
  g_parked[overlapped] = {buffer, size, it->second, Clock::now()};
  g_queries.erase(it);
  return true;
}

void Complete() {
  std::lock_guard lock(g_mutex);
  for (auto it = g_parked.begin(); it != g_parked.end();) {
    const Parked& parked = it->second;
    if (g_scans_done - parked.query.scan >= 0x80000000u && Clock::now() - parked.since < kGiveUp) {
      ++it;
      continue;
    }
    Fill(parked.buffer, parked.size, parked.query);
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(it->first, 0, 0, 0);
    it = g_parked.erase(it);
  }
}

void Cancel(uint32_t overlapped) {
  std::lock_guard lock(g_mutex);
  g_parked.erase(overlapped);
}

bool Serve(const std::string& path, std::string* body) {
  // .../users/xuid(<decimal>)/storage/titlestorage/titlegroups/<group>/data/clips/<level>/test.clip,binary
  size_t at = path.find("xuid("), skip = 5;
  if (at == std::string::npos) at = path.find("xuid%28"), skip = 7;
  if (at == std::string::npos || path.find("/data/clips/") == std::string::npos) return false;
  const uint64_t xuid = std::strtoull(path.c_str() + at + skip, nullptr, 10);
  std::lock_guard lock(g_mutex);
  for (const Entry& entry : g_list) {
    if (entry.xuid != xuid) continue;
    *body = entry.blob;
    if (std::find(g_avatars.begin(), g_avatars.end(), entry.clip.avatar) == g_avatars.end()) {
      if (g_avatars.size() >= kMaxAvatars) g_avatars.pop_front();
      g_avatars.push_back(entry.clip.avatar);
    }
    REXLOG_INFO("[clips] {} by {} goes to the game", TimeText(entry.clip.time), entry.clip.name);
    return true;
  }
  return false;
}

bool Racing() { return g_racing.load(); }

bool IsClipManifest(const uint8_t* manifest) {
  std::lock_guard lock(g_mutex);
  return std::any_of(g_avatars.begin(), g_avatars.end(), [&](const std::array<uint8_t, kManifestSize>& avatar) {
    return !std::memcmp(avatar.data(), manifest, kManifestSize);
  });
}

}  // namespace cc2::clips

REX_HOOK_RAW(sub_82B714B8) { cc2::clips::Post(ctx, base); }

REX_HOOK_RAW(sub_82B04DC8) {
  const uint32_t out = ctx.r5.u32;
  __imp__sub_82B04DC8(ctx, base);
  cc2::clips::Compressed(base, out);
}

REX_HOOK_RAW(sub_82B73870) { cc2::clips::Press(ctx, base); }

// Launch(this) starts the race with the clips added on the page.
REX_HOOK_RAW(sub_82AE32A8) {
  using namespace cc2::clips;
  const uint32_t launcher = ctx.r3.u32;
  const uint32_t clips = (Load(base, launcher + kClipsEnd) - Load(base, launcher + kClipsBegin)) / kClipSize;
  __imp__sub_82AE32A8(ctx, base);
  if (ctx.r3.u32 & 0xFF) g_racing = clips != 0;
}
