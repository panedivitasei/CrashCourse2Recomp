#include "cc2_init.h"
#include "cc2/lobby.h"
#include "cc2/lobby_menu_lua.h"
#include "cc2/avatar_fetch.h"
#include "cc2/kernel/xam/xam_avatar.h"
#include "cc2/online.h"
#include "cc2/relay.h"
#include "dcc2_tu1_map.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rexglue_guide/guide.h>

REXCVAR_DECLARE(bool, online_mode);
extern "C" REX_FUNC(sub_824C9300);
extern "C" REX_FUNC(sub_824CA4C0);
extern "C" REX_FUNC(__imp__sub_822AD240);
extern "C" REX_FUNC(sub_82AE0140);

namespace cc2::lobby {
namespace {

using Clock = std::chrono::steady_clock;
using rex::ui::VirtualKey;

constexpr int kMaxLobbyRows = 6;
constexpr auto kRefresh = std::chrono::seconds(5);
constexpr auto kHeartbeat = std::chrono::seconds(15);
constexpr auto kSessionDeadline = std::chrono::seconds(45);
constexpr auto kWaitHeartbeat = std::chrono::seconds(2);   // while a member waits for the host's session
constexpr auto kHostDeadline = std::chrono::seconds(90);   // how long a member waits for it
constexpr size_t kSessionInfoSize = 60;
constexpr uint16_t kUp = 0x1, kDown = 0x2, kStart = 0x10, kA = 0x1000, kB = 0x2000;
enum Sound { kNoSound, kMoveSound, kClickSound };
// kJoined: the server accepted the join and the game has not been told to host or join yet. kWaitHost: a member waits
// on the list screen for the host to publish its session. kJoiningGame: a member's invite join is running.
enum class Phase { kClosed, kList, kJoining, kJoined, kWaitHost, kHosting, kJoiningGame };

struct Lobby {
  std::string id, name;
  int players = 0, max_players = 0;
};

// Shared by the guest thread (Tick, pad, pointer), the UI thread (keys) and the worker (HTTP).
std::mutex g_mutex;
std::condition_variable g_wake;
Phase g_phase = Phase::kClosed;
std::vector<Lobby> g_lobbies;
bool g_listed = false;  // at least one list reply arrived since the screen opened
std::string g_message, g_hint;
int g_row = 1;
int g_sound = kNoSound;
std::string g_join_id, g_join_name;  // the lobby being joined
std::string g_joined_id;             // the lobby the server has us in
bool g_leave = false, g_fetch_now = false, g_stop = false, g_session_created = false;
Clock::time_point g_next_fetch, g_next_heartbeat, g_session_deadline, g_host_deadline, g_next_publish;
// Session plumbing for the lobby we are in: our role, the host's published XSESSION_INFO (hex) for a member, the one
// the host still has to publish, and the session the game is using.
bool g_is_host = false;
int g_pid = 0;
std::string g_target_hex, g_publish_hex;
int g_publish_tries = 0;
uint8_t g_session[kSessionInfoSize] = {};
bool g_session_valid = false;
uint16_t g_previous_buttons = 0, g_suppressed = 0;
int g_previous_vertical = 0;
Clock::time_point g_repeat_at;
std::thread g_worker;
// Set from any thread (the relay's member change, the kernel's avatar read) without g_mutex, since relay::Start joins
// the relay thread while the worker holds it.
std::atomic<bool> g_refresh_due{false};

std::atomic<bool> g_active{false};
std::atomic<rex::ui::Window*> g_window{nullptr};

// Guest-thread only.
uint32_t g_installed_state = 0, g_menu_state = 0;
int g_seen_requests = -1;
bool g_drawn = false, g_presenting = false, g_host_status_due = false;

uint32_t Read(uint32_t p) { return *REX_KERNEL_MEMORY()->TranslateVirtual<const rex::be<uint32_t>*>(p); }
void Write(uint32_t p, uint32_t v) { *REX_KERNEL_MEMORY()->TranslateVirtual<rex::be<uint32_t>*>(p) = v; }

// Rewrites a unistring in place, so every label built from it shows the new text from its first frame. The record
// keeps the active language's text at +0x40, so only the English original (`from`) is replaced, and the new text
// must fit in its buffer. Returns false until the string table is loaded.
// A loaded unistring's record (its UTF-16 text pointer at +0x40), 0 when the table or the key is not there yet.
uint32_t UnistringRecord(PPCContext& ctx, uint8_t* base, const char* key) {
  const uint32_t table = Read(0x82E98F80), manager = Read(0x82EDBAF0);
  if (!table || !manager) return 0;
  const PPCContext saved = ctx;
  const uint32_t scratch = (ctx.r1.u32 - 0x200) & ~15u;
  std::memset(base + scratch, 0, 8);
  std::memcpy(base + scratch + 16, key, std::strlen(key) + 1);
  ctx.r1.u32 = scratch - 0x80;
  ctx.r3.u64 = scratch;
  ctx.r4.u64 = table;
  ctx.r5.u64 = scratch + 16;
  sub_82AE0140(ctx, base);
  ctx.r3.u64 = manager;
  ctx.r4.u64 = *reinterpret_cast<const rex::be<uint64_t>*>(base + scratch);
  REX_CALL_INDIRECT_FUNC(Read(Read(manager) + 168));
  const uint32_t record = ctx.r3.u32;
  ctx = saved;
  return record;
}

// The UTF-16 text of a loaded unistring, 0 when the table or the key is not there yet.
uint32_t UnistringText(PPCContext& ctx, uint8_t* base, const char* key) {
  const uint32_t record = UnistringRecord(ctx, base, key);
  return record ? Read(record + 64) : 0;
}

bool SetUnistring(PPCContext& ctx, uint8_t* base, const char* key, const char* from, const char* text) {
  const uint32_t wide = UnistringText(ctx, base, key);
  if (!wide) {
    static int misses = 0;
    if (++misses == 600) REXLOG_WARN("[lobby] unistring {} not found", key);
    return false;
  }
  auto* chars = reinterpret_cast<rex::be<uint16_t>*>(base + wide);
  size_t old_length = 0;
  while (chars[old_length] != 0) ++old_length;
  const size_t from_length = std::strlen(from);
  bool english = old_length == from_length;
  for (size_t i = 0; english && i < from_length; ++i) english = chars[i] == uint16_t(uint8_t(from[i]));
  if (!english) {
    std::string current;
    for (size_t i = 0; i < old_length && i < 200; ++i) current += chars[i] < 0x80 ? char(chars[i]) : '?';
    REXLOG_DEBUG("[lobby] unistring {} left as \"{}\"", key, current);
    return true;
  }
  const size_t length = std::strlen(text);
  if (length > old_length) {
    REXLOG_WARN("[lobby] unistring {} has room for {} characters, not {}", key, old_length, length);
    return true;
  }
  for (size_t i = 0; i <= length; ++i) chars[i] = uint16_t(uint8_t(text[i]));
  REXLOG_DEBUG("[lobby] unistring {} now reads \"{}\"", key, text);
  return true;
}

// Lengthens a unistring: its text, trailing line breaks dropped, followed by `lines` (each '\n' written the way the
// text breaks its own lines), copied to a new buffer the record then points at. Returns false until the table is loaded.
bool AppendUnistring(PPCContext& ctx, uint8_t* base, const char* key, const char* lines) {
  const uint32_t record = UnistringRecord(ctx, base, key);
  const uint32_t wide = record ? Read(record + 64) : 0;
  if (!wide) return false;
  const auto* chars = reinterpret_cast<const rex::be<uint16_t>*>(base + wide);
  std::u16string text;
  bool crlf = false;
  for (size_t i = 0; chars[i] != 0; ++i) {
    crlf = crlf || chars[i] == u'\r';
    text += char16_t(uint16_t(chars[i]));
  }
  std::u16string tail;
  for (const char* c = lines; *c; ++c) {
    if (*c == '\n' && crlf) tail += u'\r';
    tail += char16_t(uint8_t(*c));
  }
  if (text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0) return true;
  while (!text.empty() && (text.back() == u'\n' || text.back() == u'\r')) text.pop_back();
  text += tail;
  const uint32_t copy = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t((text.size() + 1) * 2));
  if (!copy) return false;
  auto* out = reinterpret_cast<rex::be<uint16_t>*>(base + copy);
  for (size_t i = 0; i < text.size(); ++i) out[i] = uint16_t(text[i]);
  out[text.size()] = 0;
  Write(record + 64, copy);
  return true;
}

// The private lobby screen titles itself from these unistrings; each keeps the room of its shipped text.
bool SetLobbyTitle(PPCContext& ctx, uint8_t* base, const std::string& name) {
  struct Title { const char* key; uint32_t text; size_t room; };
  static Title titles[] = {{"common/ui/men/private_lobby_tourney", 0, 0}, {"common/ui/men/private_lobby", 0, 0}};
  bool any = false;
  for (Title& t : titles) {
    if (!t.text) {
      t.text = UnistringText(ctx, base, t.key);
      if (!t.text) continue;
      auto* chars = reinterpret_cast<const rex::be<uint16_t>*>(base + t.text);
      while (chars[t.room] != 0) ++t.room;
    }
    auto* chars = reinterpret_cast<rex::be<uint16_t>*>(base + t.text);
    const size_t length = std::min(name.size(), t.room);
    for (size_t i = 0; i < length; ++i) chars[i] = uint16_t(uint8_t(name[i]));
    chars[length] = 0;
    any = true;
  }
  if (any) REXLOG_INFO("[lobby] lobby screen title now reads \"{}\"", name);
  return any;
}

// Same calling pattern as the sign-in screen: load the chunk on L, run it, return its number (-1 on error).
int Lua(PPCContext& ctx, uint8_t* base, uint32_t L, const char* source, uint32_t* thread = nullptr) {
  const uint32_t saved_top_offset = Read(L + 8) - Read(L + 32);
  const PPCContext saved = ctx;
  const uint32_t length = uint32_t(std::strlen(source));
  const uint32_t address = (ctx.r1.u32 - length - 0x200) & ~15u;
  std::memcpy(base + address, source, length + 1);
  ctx.r1.u32 = address - 0x80;
  ctx.r3.u64 = L;
  ctx.r4.u64 = address;
  ctx.r5.u64 = length;
  sub_824CA4C0(ctx, base);
  int result = -1;
  if (ctx.r3.u32 == 0) {
    ctx.r3.u64 = L; ctx.r4.u64 = 0; ctx.r5.u64 = 1; ctx.r6.u64 = 0;
    sub_824C9300(ctx, base);
    const uint32_t value = Read(L + 8) - 16;
    if (ctx.r3.u32 == 0) {
      if (thread && Read(value + 8) == 8) *thread = Read(value);
      result = Read(value + 8) == 3 ? int(*reinterpret_cast<const rex::be<double>*>(base + value)) : 0;
    } else {
      static int errors = 0;
      const uint32_t text = Read(value);
      if (errors++ < 8 && Read(value + 8) == 4 && text) {
        REXLOG_WARN("[lobby] Lua: {}",
                    std::string(reinterpret_cast<char*>(base + text + 16), std::min(Read(text + 12), 512u)));
      }
    }
  } else {
    REXLOG_WARN("[lobby] chunk failed to load");
  }
  ctx = saved;
  Write(L + 8, Read(L + 32) + saved_top_offset);
  return result;
}

std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (uint8_t(c) >= 0x20 && uint8_t(c) < 0x7F) out += c;
  }
  return out + "\"";
}

// The server's replies are flat JSON; these read one field of one object without a full parser.
size_t ValueAt(const std::string& json, const char* key, size_t from = 0, size_t to = std::string::npos) {
  const std::string tag = std::string("\"") + key + "\"";
  size_t at = json.find(tag, from);
  if (at == std::string::npos || at >= to) return std::string::npos;
  at += tag.size();
  while (at < json.size() && (json[at] == ' ' || json[at] == '\t' || json[at] == '\r' || json[at] == '\n')) ++at;
  if (at >= json.size() || json[at] != ':') return std::string::npos;
  ++at;
  while (at < json.size() && (json[at] == ' ' || json[at] == '\t' || json[at] == '\r' || json[at] == '\n')) ++at;
  return at;
}

std::string StringAt(const std::string& json, size_t at) {
  std::string out;
  if (at == std::string::npos || at >= json.size() || json[at] != '"') return out;
  for (size_t i = at + 1; i < json.size() && json[i] != '"'; ++i) {
    char c = json[i];
    if (c == '\\' && i + 1 < json.size()) {
      c = json[++i];
      if (c == 'u') {  // non-ASCII lands as '?', the GUI font is ASCII-only here
        i += 4;
        c = '?';
      } else if (c == 'n' || c == 't') {
        c = ' ';
      }
    }
    if (uint8_t(c) >= 0x20 && uint8_t(c) < 0x7F && out.size() < 48) out += c;
  }
  return out;
}

int IntAt(const std::string& json, size_t at) {
  if (at == std::string::npos) return 0;
  return std::atoi(json.c_str() + at);
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Position of a key's value in the outermost object only, so member entries can't shadow top-level fields.
size_t TopValueAt(const std::string& json, const char* key) {
  const std::string tag = key;
  int depth = 0;
  for (size_t i = 0; i < json.size(); ++i) {
    const char c = json[i];
    if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
    } else if (c == '"') {
      size_t end = i + 1;
      while (end < json.size() && json[end] != '"') end += json[end] == '\\' ? 2 : 1;
      if (end >= json.size()) break;
      if (depth == 1 && end - i - 1 == tag.size() && json.compare(i + 1, tag.size(), tag) == 0) {
        size_t at = end + 1;
        while (at < json.size() && IsSpace(json[at])) ++at;
        if (at < json.size() && json[at] == ':') {
          ++at;
          while (at < json.size() && IsSpace(json[at])) ++at;
          return at;
        }
      }
      i = end;
    }
  }
  return std::string::npos;
}

// A string value without StringAt's display clean-up, for tokens and hex blobs.
std::string RawStringAt(const std::string& json, size_t at) {
  if (at == std::string::npos || at >= json.size() || json[at] != '"') return {};
  const size_t end = json.find('"', at + 1);
  if (end == std::string::npos || end - at - 1 > 4096) return {};
  return json.substr(at + 1, end - at - 1);
}

bool TrueAt(const std::string& json, size_t at) {
  return at != std::string::npos && json.compare(at, 4, "true") == 0;
}

std::string ToHex(const uint8_t* data, size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < size; ++i) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 15];
  }
  return out;
}

bool FromHex(const std::string& hex, uint8_t* out, size_t size) {
  if (hex.size() != size * 2) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < size; ++i) {
    const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = uint8_t(hi << 4 | lo);
  }
  return true;
}

// The XNKID at the front of a hex XSESSION_INFO, for logs.
std::string SessionIdText(const std::string& hex) { return hex.size() >= 16 ? hex.substr(0, 16) : hex; }

// Members' avatar manifests by online XUID. Own lock, since the kernel's profile reads come in on guest threads.
std::mutex g_avatar_mutex;
std::map<uint64_t, std::array<uint8_t, kAvatarManifestSize>> g_peer_avatars;

// The JSON body for join and heartbeat: our manifest as hex, so the server can hand it to the other members.
std::string AvatarBody() {
  uint8_t manifest[kAvatarManifestSize];
  if (!cc2::kernel::xam::LocalAvatarManifest(manifest)) return "{}";
  return "{\"avatar\": \"" + ToHex(manifest, sizeof(manifest)) + "\"}";
}

// Picks up each member object's "avatar" (hex X_AVATAR_METADATA) and files it under the member's XUID.
void StorePeerAvatars(const std::string& json) {
  const uint64_t self = XuidForName(online::SignedInName());
  for (size_t at = 0; (at = ValueAt(json, "username", at)) != std::string::npos;) {
    const size_t open = json.rfind('{', at), close = json.find('}', at);
    const std::string name = StringAt(json, at);
    if (open == std::string::npos || close == std::string::npos || name.empty()) continue;
    const uint64_t xuid = XuidForName(name);
    if (xuid == self) continue;
    std::array<uint8_t, kAvatarManifestSize> manifest;
    if (!FromHex(RawStringAt(json, ValueAt(json, "avatar", open, close)), manifest.data(), manifest.size())) continue;
    // Owner stamp lets XamAvatarGetAssets tell this build apart from ours and from the AI.
    for (int i = 0; i < 8; ++i) manifest[0x380 + i] = uint8_t(xuid >> (56 - 8 * i));
    bool changed = false;
    {
      std::lock_guard avatar_lock(g_avatar_mutex);
      auto [it, added] = g_peer_avatars.try_emplace(xuid, manifest);
      if (added || it->second != manifest) {
        it->second = manifest;
        changed = true;
        REXLOG_INFO("[lobby] avatar for {} ({:016X}) {}", name, xuid, added ? "received" : "updated");
      }
    }
    // Items this install lacks are fetched for rendering by the worker, between its lobby calls.
    if (changed && avatar_fetch::QueueManifest(manifest.data())) g_wake.notify_all();
  }
}

std::vector<Lobby> ParseLobbies(const std::string& json) {
  std::vector<Lobby> out;
  size_t at = ValueAt(json, "lobbies");
  if (at == std::string::npos || json[at] != '[') return out;
  const size_t end = json.find(']', at);
  while (out.size() < 16) {
    const size_t open = json.find('{', at);
    if (open == std::string::npos || open > end) break;
    const size_t close = json.find('}', open);
    if (close == std::string::npos) break;
    Lobby l;
    l.id = StringAt(json, ValueAt(json, "id", open, close));
    l.name = StringAt(json, ValueAt(json, "name", open, close));
    l.players = IntAt(json, ValueAt(json, "players", open, close));
    l.max_players = IntAt(json, ValueAt(json, "max_players", open, close));
    if (l.name.empty()) l.name = l.id;
    if (!l.id.empty()) out.push_back(std::move(l));
    at = close + 1;
  }
  return out;
}

std::string Members(const std::string& json) {
  std::string out;
  for (size_t at = 0; (at = ValueAt(json, "username", at)) != std::string::npos;) {
    if (!out.empty()) out += ", ";
    out += StringAt(json, at);
  }
  return out.empty() ? "none" : out;
}

std::string ErrorText(int status, const std::string& body) {
  if (status == -1) return "Sign in to Game Jolt to join a lobby";
  if (status == 0) return "The CC2 server can't be reached";
  const std::string error = StringAt(body, ValueAt(body, "error"));
  if (status == 409 || error == "full") return "That lobby is full";
  if (status == 404) return "That lobby no longer exists";
  return "The server said " + std::to_string(status) + (error.empty() ? "" : " (" + error + ")");
}

// Runs every server call, so the guest thread never waits on HTTP.
void ResetSessionState() {
  g_is_host = false;
  g_pid = 0;
  g_target_hex.clear();
  g_publish_hex.clear();
  g_publish_tries = 0;
  g_session_valid = false;
}

void Worker() {
  std::unique_lock lock(g_mutex);
  std::string last_members;
  int noted_host_pid = 0;
  while (true) {
    g_wake.wait_for(lock, std::chrono::milliseconds(250));
    const auto now = Clock::now();
    const bool stopping = g_stop;
    const bool session_late = !g_session_created && now >= g_session_deadline &&
                              (g_phase == Phase::kHosting || g_phase == Phase::kJoiningGame);
    const bool host_late = g_phase == Phase::kWaitHost && now >= g_host_deadline;
    if (!g_joined_id.empty() && (g_leave || stopping || session_late || host_late)) {
      const std::string id = g_joined_id;
      if (session_late) REXLOG_WARN("[lobby] the game never created its session; leaving {}", id);
      if (host_late) {
        REXLOG_WARN("[lobby] the host never published its session; leaving {}", id);
        g_phase = Phase::kList;
        g_message = "The host didn't start the game in time";
        g_fetch_now = true;
      }
      g_joined_id.clear();
      g_leave = false;
      g_session_created = false;
      ResetSessionState();
      if (g_phase == Phase::kHosting || g_phase == Phase::kJoiningGame) g_phase = Phase::kClosed;
      lock.unlock();
      relay::Stop();
      std::string body;
      const int status = online::ServerRequest("POST", "/v1/lobbies/" + id + "/leave", "{}", body);
      REXLOG_INFO("[lobby] left {} (server {})", id, status);
      lock.lock();
      last_members.clear();
      continue;
    }
    if (stopping) break;
    if (g_phase == Phase::kJoining && !g_join_id.empty()) {
      const std::string id = g_join_id, name = g_join_name;
      lock.unlock();
      std::string body;
      const int status = online::ServerRequest("POST", "/v1/lobbies/" + id + "/join", AvatarBody(), body);
      lock.lock();
      g_join_id.clear();
      if (g_phase != Phase::kJoining) {
        // Back was pressed while the request was out; undo the join on the server.
        if (status == 200) {
          g_joined_id = id;
          g_leave = true;
        }
        continue;
      }
      if (status == 200) {
        g_joined_id = id;
        g_phase = Phase::kJoined;
        g_session_created = false;
        g_next_heartbeat = Clock::now() + kHeartbeat;
        g_session_deadline = Clock::now() + kSessionDeadline;
        last_members = Members(body);
        StorePeerAvatars(body);
        ResetSessionState();
        g_pid = IntAt(body, TopValueAt(body, "pid"));
        const int host_pid = IntAt(body, TopValueAt(body, "host_pid"));
        g_is_host = g_pid > 0 && host_pid > 0 ? g_pid == host_pid : TrueAt(body, TopValueAt(body, "host"));
        const std::string relay_address = RawStringAt(body, TopValueAt(body, "relay"));
        const std::string token = RawStringAt(body, TopValueAt(body, "relay_token"));
        if (!g_is_host) g_target_hex = RawStringAt(body, TopValueAt(body, "session_info"));
        if (g_target_hex.size() != kSessionInfoSize * 2) g_target_hex.clear();
        REXLOG_INFO("[lobby] joined {} ({}) as {} (pid {}, host pid {}); members: {}", name, id,
                    g_is_host ? "host" : "member", g_pid, host_pid, last_members);
        if (!g_is_host) {
          REXLOG_INFO("[lobby] host session {}", g_target_hex.empty() ? std::string("not published yet")
                                                                     : "received: " + SessionIdText(g_target_hex));
        }
        if (!relay_address.empty() && !token.empty() && g_pid > 0) {
          relay::Start(relay_address, token, g_pid);
        } else {
          REXLOG_WARN("[lobby] the join response has no relay details (relay '{}', token {} chars, pid {}); peers "
                      "can't be reached", relay_address, token.size(), g_pid);
        }
      } else {
        g_phase = Phase::kList;
        g_message = ErrorText(status, body);
        REXLOG_WARN("[lobby] join {} failed: {}", id, g_message);
      }
      continue;
    }
    if (!g_joined_id.empty() && !g_publish_hex.empty() && now >= g_next_publish) {
      const std::string id = g_joined_id, hex = g_publish_hex;
      lock.unlock();
      std::string body;
      const int status = online::ServerRequest("POST", "/v1/lobbies/" + id + "/session",
                                               "{\"session_info\": \"" + hex + "\"}", body);
      lock.lock();
      if (status == 200) {
        REXLOG_INFO("[lobby] session {} published to lobby {}", SessionIdText(hex), id);
        if (g_publish_hex == hex) g_publish_hex.clear();
      } else if (++g_publish_tries >= 5) {
        REXLOG_ERROR("[lobby] publishing the session to {} failed (server {}); members can't join", id, status);
        g_publish_hex.clear();
      } else {
        REXLOG_WARN("[lobby] publishing the session to {} failed (server {}); retrying", id, status);
        g_next_publish = Clock::now() + std::chrono::seconds(2);
      }
      continue;
    }
    if ((g_phase == Phase::kList || g_phase == Phase::kJoining) && (g_fetch_now || now >= g_next_fetch)) {
      g_fetch_now = false;
      g_next_fetch = now + kRefresh;
      lock.unlock();
      std::string body;
      const int status = online::ServerRequest("GET", "/v1/lobbies", "", body);
      auto lobbies = status == 200 ? ParseLobbies(body) : std::vector<Lobby>{};
      lock.lock();
      if (status == 200) {
        if (!g_listed) REXLOG_INFO("[lobby] server lists {} lobbies", lobbies.size());
        g_lobbies = std::move(lobbies);
        g_listed = true;
        if (g_phase == Phase::kList) g_message = g_lobbies.empty() ? "No lobbies are open right now" : "";
      } else if (g_phase == Phase::kList) {
        g_message = ErrorText(status, body);
      }
      continue;
    }
    if (!g_joined_id.empty() && (g_refresh_due.exchange(false) | (now >= g_next_heartbeat))) {
      g_next_heartbeat = now + (g_phase == Phase::kWaitHost ? kWaitHeartbeat : kHeartbeat);
      const std::string id = g_joined_id;
      lock.unlock();
      std::string body;
      const int status = online::ServerRequest("POST", "/v1/lobbies/" + id + "/heartbeat", AvatarBody(), body);
      lock.lock();
      if (status == 200) {
        StorePeerAvatars(body);
        const std::string members = Members(body);
        if (members != last_members) {
          last_members = members;
          REXLOG_INFO("[lobby] {} members: {} (host {}, host pid {})", id, members,
                      StringAt(body, TopValueAt(body, "host")), IntAt(body, TopValueAt(body, "host_pid")));
        }
        const int host_pid = IntAt(body, TopValueAt(body, "host_pid"));
        if (id == g_joined_id && g_pid > 0 && host_pid == g_pid && !g_is_host) {
          if (g_session_valid) {
            // The game's own migration decides who hosts a running session; OnHostMigrated publishes if it picks us.
            if (noted_host_pid != host_pid) {
              REXLOG_INFO("[lobby] the server names this machine host of {}; the game's session migration decides",
                          id);
            }
          } else if (g_phase == Phase::kWaitHost || g_phase == Phase::kJoined) {
            g_is_host = true;
            g_target_hex.clear();
            g_phase = Phase::kJoined;
            g_session_created = false;
            g_session_deadline = Clock::now() + kSessionDeadline;
            REXLOG_INFO("[lobby] promoted to host by the server; hosting");
          }
        }
        noted_host_pid = host_pid;
        if (!g_is_host && id == g_joined_id && (g_target_hex.empty() || g_phase == Phase::kWaitHost)) {
          // A member still waiting takes whatever the current host published, including a new host's session.
          const std::string hex = RawStringAt(body, TopValueAt(body, "session_info"));
          if (hex.size() == kSessionInfoSize * 2 && hex != g_target_hex) {
            REXLOG_INFO("[lobby] host session {}: {}", g_target_hex.empty() ? "received" : "replaced",
                        SessionIdText(hex));
            g_target_hex = hex;
          }
        }
      } else {
        REXLOG_WARN("[lobby] heartbeat for {} failed (server {})", id, status);
      }
      continue;
    }
    if (avatar_fetch::HasPending()) {
      lock.unlock();
      avatar_fetch::FetchNext();
      lock.lock();
    }
  }
}

// Helpers below run with g_mutex held.
int RowCount() { return int(std::min<size_t>(g_lobbies.size(), kMaxLobbyRows)); }

void Close() {
  g_phase = Phase::kClosed;
  g_active = false;
  g_suppressed = g_previous_buttons;
}

void Move(int direction) {
  const int count = RowCount();
  if (g_phase != Phase::kList || count == 0) return;
  g_row = (g_row - 1 + direction + count) % count + 1;
  g_sound = kMoveSound;
  g_hint.clear();
}

bool g_cancel_due = false;  // guest thread runs the popup's cancel on its next tick

void Back(const char* source) {
  if (g_phase != Phase::kList && g_phase != Phase::kJoining && g_phase != Phase::kWaitHost) return;
  if (g_phase == Phase::kWaitHost) {
    g_leave = true;
    g_wake.notify_all();
  }
  g_sound = kClickSound;
  Close();
  g_cancel_due = true;
  REXLOG_INFO("[lobby] lobby list closed by {}", source);
}

void Activate() {
  if (g_phase != Phase::kList || g_row < 1 || g_row > RowCount()) return;
  g_sound = kClickSound;
  g_hint.clear();
  const Lobby& l = g_lobbies[g_row - 1];
  if (l.max_players > 0 && l.players >= l.max_players) {
    g_hint = "That lobby is full";
    return;
  }
  g_join_id = l.id;
  g_join_name = l.name;
  g_phase = Phase::kJoining;
  g_message = "Joining " + l.name + "...";
  g_wake.notify_all();
}

class Keys : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!g_active.load() || rexglue_guide::IsOpen()) return;
    const VirtualKey key = e.virtual_key();
    if (e.is_alt_pressed() || (key >= VirtualKey::kF1 && key <= VirtualKey::kF24)) return;
    e.set_handled(true);
    std::lock_guard lock(g_mutex);
    switch (key) {
      case VirtualKey::kUp: case VirtualKey::kW: Move(-1); break;
      case VirtualKey::kDown: case VirtualKey::kS: Move(1); break;
      case VirtualKey::kReturn: case VirtualKey::kSpace: Activate(); break;
      case VirtualKey::kEscape: case VirtualKey::kBack: Back("keyboard"); break;
      default: break;
    }
  }
  void OnKeyChar(rex::ui::KeyEvent& e) override {
    if (g_active.load() && !rexglue_guide::IsOpen()) e.set_handled(true);
  }
};

Keys g_keys;


}  // namespace

void Initialize(rex::ui::Window* window) {
  g_window.store(window);
  if (window) window->AddInputListener(&g_keys, 200);
  std::lock_guard lock(g_mutex);
  if (!g_worker.joinable()) g_worker = std::thread(Worker);
}

void Shutdown() {
  {
    std::lock_guard lock(g_mutex);
    g_stop = true;
    g_active = false;
  }
  g_wake.notify_all();
  if (g_worker.joinable()) g_worker.join();
  relay::Stop();
  if (auto* w = g_window.exchange(nullptr)) w->RemoveInputListener(&g_keys);
}

bool Active() { return g_active.load(); }

void SigninChanged() {
  std::lock_guard lock(g_mutex);
  if (!g_joined_id.empty()) g_leave = true;
  Back("a sign-in change");
  g_wake.notify_all();
}

uint32_t MainLuaState() {
  const uint32_t interp = Read(kGA_82EB7CD0);
  return interp ? Read(interp + 8) : 0;
}

int RunLua(PPCContext& ctx, uint8_t* base, uint32_t L, const char* source, uint32_t* thread) {
  return Lua(ctx, base, L, source, thread);
}

int RunMainLua(PPCContext& ctx, uint8_t* base, const char* source, uint32_t* state) {
  const uint32_t interp = Read(kGA_82EB7CD0);
  const uint32_t main_state = interp ? Read(interp + 8) : 0;
  if (state) *state = main_state;
  if (!main_state) return -2;
  return Lua(ctx, base, main_state, source);
}

void OnSessionCreated(bool host, const uint8_t* info) {
  std::lock_guard lock(g_mutex);
  g_session_created = true;
  std::memcpy(g_session, info, kSessionInfoSize);
  g_session_valid = true;
  if (g_joined_id.empty()) return;
  const std::string hex = ToHex(info, kSessionInfoSize);
  REXLOG_INFO("[lobby] the game {} session {} for lobby {}", host ? "hosts" : "joined", SessionIdText(hex),
              g_joined_id);
  if (host && g_is_host) {
    g_publish_hex = hex;
    g_publish_tries = 0;
    g_next_publish = Clock::now();
    g_wake.notify_all();
  } else if (host) {
    REXLOG_WARN("[lobby] the game created a host session although the lobby host is someone else; not publishing");
  } else if (hex != g_target_hex) {
    REXLOG_WARN("[lobby] the joined session differs from the lobby host's ({})", SessionIdText(g_target_hex));
  }
}

void OnHostMigrated(bool local_host, const uint8_t* info) {
  std::lock_guard lock(g_mutex);
  std::memcpy(g_session, info, kSessionInfoSize);
  g_session_valid = true;
  if (g_joined_id.empty()) return;
  const std::string hex = ToHex(info, kSessionInfoSize);
  g_is_host = local_host;
  if (local_host) {
    // Publishing from a member moves the server's host flag here as well.
    g_target_hex.clear();
    g_publish_hex = hex;
    g_publish_tries = 0;
    g_next_publish = Clock::now();
    g_wake.notify_all();
  } else {
    g_target_hex = hex;
    g_publish_hex.clear();
  }
}

bool CurrentSessionInfo(uint8_t* info) {
  std::lock_guard lock(g_mutex);
  if (!g_session_valid) return false;
  std::memcpy(info, g_session, kSessionInfoSize);
  return true;
}

bool JoinTarget(uint8_t* info) {
  std::lock_guard lock(g_mutex);
  if (g_joined_id.empty() || g_is_host || g_target_hex.empty()) return false;
  return FromHex(g_target_hex, info, kSessionInfoSize);
}

void CurrentSessionId(uint8_t* xnkid) {
  std::lock_guard lock(g_mutex);
  uint8_t info[kSessionInfoSize] = {};
  if (g_session_valid) {
    std::memcpy(info, g_session, sizeof(info));
  } else if (!g_target_hex.empty()) {
    FromHex(g_target_hex, info, sizeof(info));
  }
  std::memcpy(xnkid, info, 8);
}

void OnSessionDeleted() {
  std::lock_guard lock(g_mutex);
  g_session_valid = false;
  if (g_joined_id.empty()) return;
  REXLOG_INFO("[lobby] the game deleted its session; leaving lobby {}", g_joined_id);
  g_leave = true;
  g_wake.notify_all();
}

void FilterInput(uint32_t user, rex::input::X_INPUT_GAMEPAD* pad) {
  if (user || !pad) return;
  std::lock_guard lock(g_mutex);
  const uint16_t buttons = pad->buttons;
  const uint16_t pressed = buttons & ~g_previous_buttons;
  g_previous_buttons = buttons;
  if (!g_active.load()) {
    g_suppressed &= buttons;
    pad->buttons = buttons & ~g_suppressed;
    g_previous_vertical = 0;
    return;
  }
  const auto now = Clock::now();
  const int vertical = (buttons & kDown || int16_t(pad->thumb_ly) < -16000) ? 1
                       : (buttons & kUp || int16_t(pad->thumb_ly) > 16000) ? -1 : 0;
  if (vertical && (vertical != g_previous_vertical || now >= g_repeat_at)) {
    Move(vertical);
    g_repeat_at = now + std::chrono::milliseconds(vertical != g_previous_vertical ? 350 : 150);
  }
  g_previous_vertical = vertical;
  if (pressed & (kA | kStart)) Activate();
  else if (pressed & kB) Back("controller B");
  pad->buttons = 0;
  pad->left_trigger = 0;
  pad->right_trigger = 0;
  pad->thumb_lx = 0;
  pad->thumb_ly = 0;
  pad->thumb_rx = 0;
  pad->thumb_ry = 0;
}

void Pointer(PPCContext& ctx, uint8_t* base, float x, float y, int button) {
  if (!g_active.load() || !g_menu_state || !g_drawn) return;
  int count;
  {
    std::lock_guard lock(g_mutex);
    count = RowCount();
  }
  char text[96];
  std::snprintf(text, sizeof(text), "return CC2LobbyHit(%.9g,%.9g,%d)", x, y, count);
  const int row = Lua(ctx, base, g_menu_state, text);
  std::lock_guard lock(g_mutex);
  if (button == 1) {
    Back("right mouse button");
    return;
  }
  if (row == -1 && button == 0) {
    Back("the Cancel prompt");
    return;
  }
  if (g_phase != Phase::kList || row < 1 || row > RowCount()) return;
  if (row != g_row) {
    g_row = row;
    g_sound = kMoveSound;
  }
  if (button == 0) Activate();
}

// After the last track of a Time Trial set the game returns to the main menu without calling gui.LoadMap, so the
// menu shows over a scene with no actors at all. A level load empties the scene for about a second, so five seconds
// of an empty scene with the main menu GUI up means the map is missing; this loads it the way the game's own
// back-from-game handler does.
uint32_t SceneActorCount(uint8_t* base) {
  const uint32_t scene = Read(0x82EDBB1C);
  if (!scene) return 0;
  auto index16 = [&](uint32_t address) { return int16_t(*reinterpret_cast<const rex::be<int16_t>*>(base + address)); };
  int16_t index = index16(scene + 164334);
  const int16_t tail = index16(scene + 164336);
  uint32_t actors = 0;
  for (unsigned visited = 0; index >= 0 && index < 6000 && visited < 6000; ++visited) {
    const uint32_t node = scene + 54044 + uint32_t(index) * 16;
    if (Read(node + 4)) ++actors;
    if (index == tail) break;
    index = index16(node + 14);
  }
  return actors;
}

void MenuMapWatchdog(PPCContext& ctx, uint8_t* base, uint32_t main_state) {
  // Checked four times a second; two misses in a row keep a menu that is still being built out of it.
  constexpr int kInterval = 15, kMisses = 2;
  static int frames = 0, misses = 0, pans = 0;
  if (!main_state || !Read(0x82EDBB1C)) {
    frames = misses = pans = 0;
    return;
  }
  if (++frames < kInterval) return;
  frames = 0;
  const uint32_t L = g_installed_state == main_state && g_menu_state ? g_menu_state : main_state;
  // 2 when the main menu is up and believes its map is loaded (mainmenumaploaded 1) while the map's camera actor is
  // gone: one way back from a level deletes the map and never reloads it. 1 is a healthy menu, 0 anything else.
  const int state = Lua(ctx, base, L,
                        "if not (gui and gui.LoadMap and gui.main and gui.main.options and scene and resources) then return 0 end "
                        "if mainmenumaploaded~=1 or (Exist and Exist('Game')) or not resources.IsReady() then return 0 end "
                        "local ok,c=pcall(function() return scene['mm/camera/center'] end) "
                        "if not ok then return 0 end return c and 1 or 2");
  if (state == 1 && pans > 0) {
    // A pan issued with the load uses the camera actor before it has settled and frames the globe too close, so the
    // game's own back-from-level framing is applied on the next two checks instead.
    --pans;
    Lua(ctx, base, L,
        "pcall(function() scene['mm/logo/logo'].Opacity=0 scene['mm/logo/glove'].Opacity=0 end) "
        "gui.CameraPanTo('mm/camera/center',0) return 1");
  }
  // A loaded level has hundreds of actors; the broken menu has only the avatar's.
  if (state != 2 || SceneActorCount(base) > 200) {
    misses = 0;
    return;
  }
  if (++misses < kMisses) return;
  misses = 0;
  // The first step the game's own back-from-level handler takes; hiding the logo props and centring the camera follow.
  const int result = Lua(ctx, base, L,
                         "if camera then camera.Fov=45 end gui.LoadMap('mainmenu/mainmenumap.wxm') mainmenumaploaded=1 "
                         "return 10");
  pans = 2;
  REXLOG_INFO("[lobby] main menu was up without its map; reloaded it (result {}, {} actors now)", result,
              SceneActorCount(base));
}

void Tick(PPCContext& ctx, uint8_t* base) {
  // Retail's English text says "extra star" for a prize that is always two or more; the other prize objectives say
  // "stars". Dropping "a " makes room for the plural in the same buffer.
  static bool pluralised = false;
  if (!pluralised) {
    pluralised = SetUnistring(ctx, base, "missions/star_bronze",
                              "Grab @:gui.MissionVariable; stars and finish with at least a bronze for "
                              "@:gui.MissionStarsPrize; extra star.",
                              "Grab @:gui.MissionVariable; stars and finish with at least bronze for "
                              "@:gui.MissionStarsPrize; extra stars.");
  }
  // The credits page text ends with Special Thanks; JMstudios follows as one more section.
  static bool credited = false;
  if (!credited) {
    credited = AppendUnistring(ctx, base, "common/ui/men/h_opt/cred/main",
                               "\n\n-=JMstudios=-\nPane\nSherlyn\nJosh\nMicah\n\n");
  }
  // Sign-in prompt text for the Game Jolt art in sys/msg/xboxlive.dds.
  static bool rebranded = false;
  if (!rebranded) {
    rebranded = SetUnistring(ctx, base, "common/ui/sys/msg/xbox/xlive_needed",
                             "You must be signed into Xbox LIVE to play this game.",
                             "Sign in to Game Jolt to play online.");
  }
  const uint32_t interp = Read(kGA_82EB7CD0);
  const uint32_t main_state = interp ? Read(interp + 8) : 0;
  MenuMapWatchdog(ctx, base, main_state);
  if (!REXCVAR_GET(online_mode)) return;
  if (!main_state) return;
  if (g_installed_state != main_state) {
    g_menu_state = 0;
    g_drawn = false;
    g_presenting = false;
    g_seen_requests = -1;
    if (Lua(ctx, base, main_state, "CC2LobbyThread=coroutine.create(function() end); return CC2LobbyThread",
            &g_menu_state) < 0 || !g_menu_state) {
      return;
    }
    if (Lua(ctx, base, g_menu_state, kLobbyScript) < 0) return;
    g_installed_state = main_state;
    REXLOG_DEBUG("[lobby] menu script installed");
  }
  // The Quick Match button's label is this unistring; renaming it before the menu builds avoids a flash of the old text.
  static bool relabelled = false;
  if (!relabelled) {
    relabelled = SetUnistring(ctx, base, "common/ui/men/opt/find_online", "QUICK MATCH", "JOIN MATCH");
  }
  static std::string titled;
  std::string title;
  {
    std::lock_guard lock(g_mutex);
    title = g_join_name;
  }
  if (!title.empty() && title != titled && SetLobbyTitle(ctx, base, title)) titled = title;
  // Keeps Quick Match hooked; the count goes up each time the button is used.
  const int requests = Lua(ctx, base, g_menu_state, "CC2LobbyInstall(); return CC2LobbyRequests");
  bool open_now = false;
  if (requests >= 0) {
    if (g_seen_requests >= 0 && requests > g_seen_requests) open_now = true;
    g_seen_requests = requests;
  }
  std::string call;
  bool host_now = false, join_now = false, cancel_now = false;
  {
    std::lock_guard lock(g_mutex);
    // Join Match back on the menu means the game dropped a join or host attempt that never made its session.
    if (open_now && !g_session_created && (g_phase == Phase::kJoiningGame || g_phase == Phase::kHosting)) {
      REXLOG_WARN("[lobby] Join Match selected while the game never made its session; leaving and reopening the list");
      g_leave = true;
      g_phase = Phase::kClosed;
    }
    if (open_now && g_phase == Phase::kClosed) {
      g_phase = Phase::kList;
      g_active = true;
      g_row = 1;
      g_listed = false;
      g_hint.clear();
      g_message = "Fetching lobbies...";
      g_fetch_now = true;
      g_wake.notify_all();
      REXLOG_INFO("[lobby] Join Match selected; lobby list opened");
      // No account: Game Jolt sign-in opens over the list.
      if (online::SignedInName().empty()) online::ShowSignIn();
    } else if (open_now) {
      REXLOG_WARN("[lobby] Join Match selected while a lobby is still being joined; ignored");
    }
    cancel_now = g_cancel_due;
    g_cancel_due = false;
    if (g_phase == Phase::kJoined && g_is_host) {
      host_now = true;
      g_phase = Phase::kHosting;
      g_active = false;
      g_suppressed = g_previous_buttons;
    } else if (g_phase == Phase::kJoined) {
      // A member waits on the list screen until the host's session is known, then takes the game's join route.
      g_phase = Phase::kWaitHost;
      g_host_deadline = Clock::now() + kHostDeadline;
      g_next_heartbeat = Clock::now();
      g_message = "Waiting for the host to open the game...";
      g_wake.notify_all();
      REXLOG_INFO("[lobby] waiting for the lobby host's session");
    }
    if (g_phase == Phase::kWaitHost && !g_target_hex.empty()) {
      join_now = true;
      g_phase = Phase::kJoiningGame;
      g_session_deadline = Clock::now() + kSessionDeadline;
      g_active = false;
      g_suppressed = g_previous_buttons;
    }
    if (g_phase == Phase::kList || g_phase == Phase::kJoining || g_phase == Phase::kWaitHost) {
      const int count = RowCount();
      g_row = std::clamp(g_row, 1, std::max(count, 1));
      std::string rows[kMaxLobbyRows];
      for (int i = 0; i < count; ++i) {
        const Lobby& l = g_lobbies[i];
        rows[i] = l.name + ":   " + std::to_string(l.players) + "/" + std::to_string(l.max_players);
      }
      const std::string status = !g_hint.empty() ? g_hint : g_message;
      call = "return CC2LobbyTick(true," + std::to_string(g_row) + "," + std::to_string(count) + "," +
             Quote(status) + "," + std::to_string(g_sound);
      for (const auto& r : rows) call += "," + Quote(r);
      call += ")";
      g_sound = kNoSound;
    }
  }
  if (g_host_status_due) {
    g_host_status_due = false;
    Lua(ctx, base, g_menu_state, "return CC2LobbyHostStatus()");
  }
  if (cancel_now) {
    Lua(ctx, base, g_menu_state, "return CC2LobbyCancel()");
  }
  if (call.empty()) {
    if (g_presenting) {
      Lua(ctx, base, g_menu_state, "return CC2LobbyTick(false)");
      g_presenting = false;
      g_drawn = false;
    }
    if (join_now) {
      // Quick Match only takes public slots and the Private Match host has none left, so a member accepts an invite
      // instead (XN_LIVE_INVITE_ACCEPTED, param = user index). XInviteGetAcceptedInfo then returns the host's session.
      Lua(ctx, base, g_menu_state, "return CC2LobbyCancel()");
      if (auto* kernel = rex::system::kernel_state()) kernel->BroadcastNotification(0x02000002, 0);
      REXLOG_INFO("[lobby] joining the host's session through the game's invite path");
    }
    if (host_now) {
      const int hosted = Lua(ctx, base, g_menu_state, "return CC2LobbyHost()");
      REXLOG_INFO("[lobby] hosting the game's session through its Private Match path ({})",
                  hosted == 1 ? "started" : "no matchmaking popup to host on");
      g_host_status_due = hosted == 1;
      if (hosted != 1) {
        std::lock_guard lock(g_mutex);
        g_leave = true;
        g_wake.notify_all();
      }
    }
    return;
  }
  g_presenting = true;
  const int result = Lua(ctx, base, g_menu_state, call.c_str());
  g_drawn = result == 1;
  // A list the GUI never builds would hold the input with nothing on screen, so it gives up and cancels.
  static int failures = 0;
  failures = result < 0 ? failures + 1 : 0;
  if (failures >= 30) {
    failures = 0;
    Lua(ctx, base, g_menu_state, "return CC2LobbyTick(false)");
    g_presenting = false;
    g_drawn = false;
    std::lock_guard lock(g_mutex);
    Close();
    g_cancel_due = true;
    REXLOG_ERROR("[lobby] lobby list could not be built; see the Lua warnings above");
  }
}

uint64_t XuidForName(const std::string& name) {
  uint64_t hash = 0xCBF29CE484222325ULL;
  for (unsigned char c : name) {
    hash ^= uint64_t(c >= 'A' && c <= 'Z' ? c + 32 : c);
    hash *= 0x100000001B3ULL;
  }
  return 0x0009000000000000ULL | (hash & 0x0000FFFFFFFFFFFFULL);
}

bool PeerAvatar(uint64_t xuid, uint8_t* manifest) {
  std::lock_guard lock(g_avatar_mutex);
  const auto it = g_peer_avatars.find(xuid);
  if (it == g_peer_avatars.end()) return false;
  std::memcpy(manifest, it->second.data(), it->second.size());
  return true;
}

bool IsPeerXuid(uint64_t xuid) {
  std::lock_guard lock(g_avatar_mutex);
  return g_peer_avatars.count(xuid) != 0;
}

void RequestPeerRefresh() {
  g_refresh_due = true;
  g_wake.notify_all();
}

bool FetchPeerAvatar(uint64_t xuid, uint8_t* manifest, int budget_ms) {
  if (PeerAvatar(xuid, manifest)) return true;
  // A peer that sends no avatar would stall every read, so a miss is not retried for a while.
  static std::mutex miss_mutex;
  static std::map<uint64_t, Clock::time_point> last_miss;
  {
    std::lock_guard lock(miss_mutex);
    const auto it = last_miss.find(xuid);
    if (it != last_miss.end() && Clock::now() - it->second < std::chrono::seconds(10)) return false;
  }
  std::string id;
  {
    std::lock_guard lock(g_mutex);
    id = g_joined_id;
  }
  if (id.empty()) return false;
  const auto start = Clock::now();
  std::string body;
  const int status =
      online::ServerRequest("POST", "/v1/lobbies/" + id + "/heartbeat", AvatarBody(), body, budget_ms);
  if (status == 200) StorePeerAvatars(body);
  const bool found = PeerAvatar(xuid, manifest);
  REXLOG_INFO("[lobby] avatar fetch for {:016X}: {} after {} ms (server {})", xuid, found ? "found" : "missing",
              std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count(), status);
  if (!found) {
    std::lock_guard lock(miss_mutex);
    last_miss[xuid] = Clock::now();
  }
  return found;
}

void FillLocalXnAddr(uint8_t* xnaddr) { relay::FillXnAddr(relay::AddressOctet(), xnaddr); }

void RandomBytes(void* out, size_t size) {
  static std::mutex mutex;
  static std::mt19937_64 engine{std::random_device{}() ^
                                uint64_t(Clock::now().time_since_epoch().count())};
  std::lock_guard lock(mutex);
  auto* bytes = static_cast<uint8_t*>(out);
  for (size_t i = 0; i < size; i += 8) {
    const uint64_t v = engine();
    std::memcpy(bytes + i, &v, std::min<size_t>(8, size - i));
  }
}

}  // namespace cc2::lobby

// Online lobby AddPlayer (0x82B0E4F0) reads five Lua results through this converter and bails unless all five are
// set; TU1 expects a fifth loadout group with Refresh, but the disc lobby script returns four, so it gets the gamertag group.
REX_HOOK_RAW(sub_822AD240) {
  const uint32_t gamertag_out = ctx.r6.u32, loadout_out = ctx.r9.u32;
  __imp__sub_822AD240(ctx, base);
  if (!gamertag_out || !loadout_out) return;
  const uint32_t gamertag = cc2::lobby::Read(gamertag_out);
  if (gamertag && !cc2::lobby::Read(loadout_out)) cc2::lobby::Write(loadout_out, gamertag);
}
