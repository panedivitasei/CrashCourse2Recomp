#include "cc2/online.h"
#include "cc2/lobby.h"
#include "cc2/kernel/xam/local_players.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <jmlive/jmlive.h>
#include <rexglue_guide/guide.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

REXCVAR_DEFINE_BOOL(online_mode, true, "Online",
                    "false: full offline play, with no Game Jolt sign-in, lobbies or trophies");
REXCVAR_DEFINE_STRING(cc2_server_url, "https://cc2.jm-studios.org", "Online",
                      "The JMstudios CC2 server");

namespace cc2::online {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_live{false};
std::mutex g_mutex;
std::condition_variable g_wake;
bool g_stop = false;
std::string g_name;  // Game Jolt username, cached for the game thread
std::deque<uint32_t> g_unlocks;
std::thread g_worker;
rex::Runtime* g_runtime = nullptr;
rex::system::AchievementListenerHandle g_listener = 0;

// XamShowSigninUI in flight: waiting for the Guide to open, then to close.
enum class Signin { kNone, kOpening, kOpen };
Signin g_signin = Signin::kNone;
Clock::time_point g_signin_at;

std::filesystem::path SyncedFile() { return rex::filesystem::GetExecutableFolder() / "trophies-synced.txt"; }

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// One JSON request to the server; returns the HTTP status (0 when the server can't be reached) and the body.
int Send(const wchar_t* verb, const std::string& path, const std::string& json, const std::string& bearer,
         std::string& body, int timeout_ms = 0) {
  const std::string base = REXCVAR_GET(cc2_server_url);
  URL_COMPONENTS url{sizeof(url)};
  wchar_t host[256] = {};
  url.lpszHostName = host;
  url.dwHostNameLength = 255;
  const std::wstring wbase = Wide(base);
  if (!WinHttpCrackUrl(wbase.c_str(), 0, 0, &url)) return 0;
  int status = 0;
  HINTERNET session = WinHttpOpen(L"CC2Recompiled/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET connect = session ? WinHttpConnect(session, host, url.nPort, 0) : nullptr;
  HINTERNET request = connect ? WinHttpOpenRequest(connect, verb, Wide(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   url.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                              : nullptr;
  if (request) {
    // A budgeted call splits it across resolve, connect, send and the reply, so the whole request stays inside it.
    if (timeout_ms > 0) {
      WinHttpSetTimeouts(request, timeout_ms / 5, timeout_ms / 5, timeout_ms / 5, timeout_ms * 2 / 5);
    } else {
      WinHttpSetTimeouts(request, 5000, 5000, 10000, 10000);
    }
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!bearer.empty()) headers += L"Authorization: Bearer " + Wide(bearer) + L"\r\n";
    if (WinHttpSendRequest(request, headers.c_str(), DWORD(-1), const_cast<char*>(json.data()), DWORD(json.size()),
                           DWORD(json.size()), 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
      DWORD code = 0, size = sizeof(code);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &size,
                          nullptr);
      status = int(code);
      char buffer[1024];
      DWORD read = 0;
      while (body.size() < (1u << 20) && WinHttpReadData(request, buffer, sizeof(buffer), &read) && read) {
        body.append(buffer, read);
      }
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  if (session) WinHttpCloseHandle(session);
  return status;
}

// The notifications a console sends once a profile reaches Xbox Live: sign-in changed, the guide closed, and the
// Live link up (both the SYS and the LIVE-area forms the game listens for).
void GoLive() {
  if (g_live.exchange(true)) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return;
  kernel->BroadcastNotification(0x0000000A, cc2::kernel::xam::SignedInUserMask());
  kernel->BroadcastNotification(0x00000009, 0);
  kernel->BroadcastNotification(0x00000014, 1);
  kernel->BroadcastNotification(0x02000001, 0x001510F0);
  REXLOG_DEBUG("[online] the game now sees the profile on Xbox Live");
}

// The console's notifications for a profile leaving Xbox Live: the Live link down, then sign-in changed.
void LeaveLive() {
  if (!g_live.exchange(false)) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return;
  kernel->BroadcastNotification(0x02000001, 0x001510F1);  // XONLINE_S_LOGON_DISCONNECTED
  kernel->BroadcastNotification(0x00000014, 0);
  kernel->BroadcastNotification(0x0000000A, cc2::kernel::xam::SignedInUserMask());
  REXLOG_DEBUG("[online] the game no longer sees the profile on Xbox Live");
}

std::set<uint32_t> ReadSynced() {
  std::set<uint32_t> ids;
  std::ifstream in(SyncedFile());
  for (uint32_t id; in >> id;) ids.insert(id);
  return ids;
}

void MarkSynced(uint32_t id) {
  std::ofstream out(SyncedFile(), std::ios::app);
  out << id << '\n';
}

// Sends one unlock; true once the server has it (awarded now or earlier), so it is never sent again.
bool SendUnlock(uint32_t id, const std::string& session) {
  std::string body;
  const int status = Send(L"POST", "/v1/trophy", "{\"achievement_id\": " + std::to_string(id) + "}", session, body);
  if (status != 200) return false;
  if (body.find("true") != std::string::npos) REXLOG_INFO("[online] trophy for achievement {} awarded", id);
  return true;
}

// Sends unlocks the server doesn't have yet.
void CatchUp(const std::string& session) {
  if (!g_runtime || !g_runtime->kernel_state()) return;
  const auto synced = ReadSynced();
  auto& manager = g_runtime->kernel_state()->achievements();
  for (const auto& info : g_runtime->kernel_state()->loaded_achievements()) {
    if (manager.IsUnlocked(info.id) && !synced.count(info.id) && SendUnlock(info.id, session)) MarkSynced(info.id);
  }
}

// Follows the Guide's account: Live and trophies on sign-in, off Live on sign-out, offline when a sign-in closes
// without one.
void Worker() {
  std::string signed_in_as;
  Clock::time_point relive_at{};  // after a profile switch: back on Live once the game has seen the sign-out
  std::unique_lock lock(g_mutex);
  while (!g_stop) {
    g_wake.wait_for(lock, std::chrono::milliseconds(250));
    if (g_stop) break;
    lock.unlock();
    const jmlive::Account account = jmlive::GameJolt();
    {
      std::lock_guard name_lock(g_mutex);
      g_name = account.username;
    }
    if (!account.session.empty() && account.username != signed_in_as) {
      // Already on Live as the local profile or another account: a sign-in change, as on a console.
      const bool switched = g_live.load();
      signed_in_as = account.username;
      REXLOG_INFO("[online] signed in to Game Jolt as {}", signed_in_as);
      if (switched) {
        lobby::SigninChanged();
        LeaveLive();
        relive_at = Clock::now() + std::chrono::seconds(2);
      } else {
        GoLive();
      }
      CatchUp(account.session);
    } else if (account.session.empty() && !signed_in_as.empty()) {
      REXLOG_INFO("[online] signed out of Game Jolt");
      signed_in_as.clear();
      relive_at = {};
      lobby::SigninChanged();
      LeaveLive();
    }
    if (relive_at != Clock::time_point{} && Clock::now() >= relive_at) {
      relive_at = {};
      GoLive();
    }
    const bool guide_open = rexglue_guide::IsOpen();
    lock.lock();
    if (g_signin != Signin::kNone) {
      if (!account.session.empty()) {
        g_signin = Signin::kNone;
      } else if (g_signin == Signin::kOpening && guide_open) {
        g_signin = Signin::kOpen;
      } else if ((g_signin == Signin::kOpen && !guide_open) ||
                 (g_signin == Signin::kOpening && Clock::now() - g_signin_at > std::chrono::seconds(5))) {
        g_signin = Signin::kNone;
        REXLOG_INFO("[online] sign-in closed without a Game Jolt account; playing offline");
        GoLive();
      }
    }
    while (!account.session.empty() && !g_unlocks.empty()) {
      const uint32_t id = g_unlocks.front();
      lock.unlock();
      const bool sent = SendUnlock(id, account.session);
      lock.lock();
      if (!sent) break;  // retried on the next wake; the synced file keeps it pending across runs
      g_unlocks.pop_front();
      MarkSynced(id);
    }
  }
}

}  // namespace

void Start(rex::Runtime* runtime) {
  g_runtime = runtime;
  if (!REXCVAR_GET(online_mode)) return;
  if (runtime && runtime->kernel_state()) {
    // First-time unlocks only; ones saved from earlier runs are caught up by the worker after sign-in.
    g_listener = runtime->kernel_state()->achievements().RegisterUnlockCallback(
        [](const rex::system::AchievementEvent& e) {
          std::lock_guard lock(g_mutex);
          g_unlocks.push_back(e.achievement.id);
          g_wake.notify_all();
        });
  }
  g_worker = std::thread(Worker);
}

void Stop() {
  {
    std::lock_guard lock(g_mutex);
    g_stop = true;
  }
  g_wake.notify_all();
  if (g_worker.joinable()) g_worker.join();
  if (g_listener && g_runtime && g_runtime->kernel_state()) {
    g_runtime->kernel_state()->achievements().UnregisterCallback(g_listener);
  }
}

void RequestSignIn(uint32_t flags, uint32_t mask) {
  if (!REXCVAR_GET(online_mode)) return;
  REXLOG_INFO("[online] the game asked for sign-in (flags {:#x}, mask {:#x}); opening Game Jolt sign-in", flags, mask);
  {
    std::lock_guard lock(g_mutex);
    g_signin = Signin::kOpening;
    g_signin_at = Clock::now();
  }
  jmlive::ShowSignIn();
}

void ShowSignIn() { jmlive::ShowSignIn(); }

int ServerRequest(const char* verb, const std::string& path, const std::string& json, std::string& body,
                  int timeout_ms) {
  const jmlive::Account account = jmlive::GameJolt();
  if (account.session.empty()) return -1;
  return Send(Wide(verb).c_str(), path, json, account.session, body, timeout_ms);
}

std::string SignedInName() {
  std::lock_guard lock(g_mutex);
  return g_name;
}

bool ReportLive() { return !REXCVAR_GET(online_mode) || g_live.load(); }

}  // namespace cc2::online
