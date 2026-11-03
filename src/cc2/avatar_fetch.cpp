#include "cc2/avatar_fetch.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "cc2/kernel/xam/xam_avatar.h"
#include "kernel/xam/avatars/closet.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

REXCVAR_DEFINE_STRING(avatar_marketplace_server, "https://avatars.jm-studios.org", "Online",
                      "Avatar marketplace server that lobby peers' items are fetched from for rendering");
REXCVAR_DEFINE_STRING(avatar_marketplace_key, "", "Online",
                      "Shared key sent to the avatar marketplace server as X-Marketplace-Key; empty uses the one "
                      "JMinstaller seals into the exe");
REXCVAR_DEFINE_STRING(avatar_cache_dir, "", "Online",
                      "Folder for lobby peers' downloaded items. Empty = <avatar_data_dir>\\avatar_cache.");

namespace cc2::avatar_fetch {
namespace {

using rex::avatars::AssetId;

// The key jm-pack appends to the shipped exe: [key XOR pad][u32 length]["JMKEY1"]. Obfuscation, not secrecy, so the
// server still has to treat the key as public. The pad matches jm-pack's KEY_PAD.
std::string KeyFromExe() {
  static const unsigned char kPad[] = {0x4a, 0x4d, 0x73, 0x74, 0x75, 0x64, 0x69, 0x6f, 0x73, 0x2e, 0x6d,
                                       0x61, 0x72, 0x6b, 0x65, 0x74, 0x70, 0x6c, 0x61, 0x63, 0x65, 0x2e,
                                       0x76, 0x31, 0x20, 0x4a, 0x4d, 0x73, 0x74, 0x75, 0x64, 0x69};
  constexpr char kMagic[] = "JMKEY1";
  constexpr size_t kMagicLen = sizeof(kMagic) - 1;
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (!n || n == MAX_PATH) return {};
  std::ifstream exe(std::filesystem::path(path), std::ios::binary);
  unsigned char tail[4 + kMagicLen];
  if (!exe.seekg(-std::streamoff(sizeof tail), std::ios::end) ||
      !exe.read(reinterpret_cast<char*>(tail), sizeof tail) || std::memcmp(tail + 4, kMagic, kMagicLen) != 0) {
    return {};
  }
  const uint32_t len =
      uint32_t(tail[0]) | (uint32_t(tail[1]) << 8) | (uint32_t(tail[2]) << 16) | (uint32_t(tail[3]) << 24);
  if (!len || len > 256 || !exe.seekg(-std::streamoff(sizeof tail + len), std::ios::end)) return {};
  std::string key(len, char(0));
  if (!exe.read(key.data(), len)) return {};
  for (size_t i = 0; i < key.size(); ++i) key[i] = char(key[i] ^ kPad[i % sizeof kPad]);
  return key;
}

std::string MarketplaceKey() {
  static const std::string sealed = KeyFromExe();
  const std::string key = REXCVAR_GET(avatar_marketplace_key);
  return key.empty() ? sealed : key;
}

std::mutex g_mutex;
std::condition_variable g_done;
std::deque<std::pair<std::string, uint32_t>> g_queue;  // guid and title id, waiting for FetchNext
std::set<std::string> g_busy;        // queued or downloading
std::set<std::string> g_missing;     // the server has no such item; not asked again this run
std::filesystem::path g_dir;

std::filesystem::path CacheDir() {
  std::lock_guard lock(g_mutex);
  if (g_dir.empty()) {
    const std::string configured = REXCVAR_GET(avatar_cache_dir);
    g_dir = configured.empty()
                ? std::filesystem::path(cc2::kernel::xam::AvatarDataDir()) / "avatar_cache"
                : std::filesystem::path(configured);
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
  }
  return g_dir;
}

std::filesystem::path CachePath(const std::string& guid) { return CacheDir() / (guid + ".bin"); }

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// Plain GET with the marketplace key; returns the HTTP status (0 when unreachable) and the body.
int Get(const std::string& url_text, std::string& body) {
  URL_COMPONENTS url{sizeof(url)};
  wchar_t host[256] = {}, path[1024] = {};
  url.lpszHostName = host;
  url.dwHostNameLength = 255;
  url.lpszUrlPath = path;
  url.dwUrlPathLength = 1023;
  const std::wstring wide = Wide(url_text);
  if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &url)) return 0;
  int status = 0;
  HINTERNET session = WinHttpOpen(L"CC2Recompiled/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET connect = session ? WinHttpConnect(session, host, url.nPort, 0) : nullptr;
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   url.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                              : nullptr;
  if (request) {
    WinHttpSetTimeouts(request, 5000, 5000, 10000, 20000);
    const std::string key = MarketplaceKey();
    const std::wstring headers = key.empty() ? std::wstring() : L"X-Marketplace-Key: " + Wide(key) + L"\r\n";
    if (WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                           headers.empty() ? 0 : DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
      DWORD code = 0, size = sizeof(code);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &size,
                          nullptr);
      status = int(code);
      char buffer[16384];
      DWORD read = 0;
      while (body.size() < (64u << 20) && WinHttpReadData(request, buffer, sizeof(buffer), &read) && read) {
        body.append(buffer, read);
      }
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  if (session) WinHttpCloseHandle(session);
  return status;
}

uint32_t TitleIdOf(const AssetId& id) {
  return (uint32_t(id.d[4]) << 24) | (uint32_t(id.d[5]) << 16) | (uint32_t(id.d[6]) << 8) | uint32_t(id.d[7]);
}

}  // namespace

bool QueueManifest(const uint8_t* manifest) {
  // Body, head and the 13 worn components sit at 0x120..0x300, 32 bytes each with the item id first.
  bool queued = false;
  for (size_t offset = 0x120; offset < 0x300; offset += 32) {
    AssetId id;
    std::memcpy(&id, manifest + offset, sizeof(id));
    static const uint8_t kZeroTail[8] = {};
    if (id.is_zero() || rex::avatars::IsStockPackId(id) || !std::memcmp(id.d, kZeroTail, sizeof(kZeroTail))) {
      continue;
    }
    if (rex::avatars::GetCloset().Find(id)) continue;
    const std::string guid = id.to_string();
    std::error_code ec;
    if (std::filesystem::exists(CachePath(guid), ec)) continue;
    std::lock_guard lock(g_mutex);
    if (g_missing.count(guid) || !g_busy.insert(guid).second) continue;
    g_queue.emplace_back(guid, TitleIdOf(id));
    queued = true;
  }
  return queued;
}

bool HasPending() {
  std::lock_guard lock(g_mutex);
  return !g_queue.empty();
}

bool FetchNext() {
  std::string guid;
  uint32_t title_id = 0;
  {
    std::lock_guard lock(g_mutex);
    if (g_queue.empty()) return false;
    guid = g_queue.front().first;
    title_id = g_queue.front().second;
    g_queue.pop_front();
  }
  std::string server = REXCVAR_GET(avatar_marketplace_server);
  while (!server.empty() && server.back() == '/') server.pop_back();
  const std::string url = server + fmt::format("/live/{:08x}/avataritems/{}.acp", title_id, guid);
  std::string body;
  const int status = server.empty() ? 0 : Get(url, body);
  bool stored = false;
  if (status == 200 && !body.empty()) {
    // Written under a temp name first so a concurrent reader never sees half a blob.
    const auto path = CachePath(guid);
    auto temp = path;
    temp += ".part";
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      out.write(body.data(), std::streamsize(body.size()));
      stored = bool(out);
    }
    std::error_code ec;
    if (stored) std::filesystem::rename(temp, path, ec);
    stored = stored && !ec;
    if (!stored) std::filesystem::remove(temp, ec);
  }
  if (stored) {
    REXLOG_INFO("[avatar] fetched peer item {} ({:#x} bytes) for rendering", guid, body.size());
  } else {
    REXLOG_WARN("[avatar] peer item {} unavailable from the marketplace server (status {})", guid, status);
  }
  {
    std::lock_guard lock(g_mutex);
    g_busy.erase(guid);
    // Only a definite answer sticks; a network failure may be retried when the manifest comes round again.
    if (!stored && status >= 400 && status < 500) g_missing.insert(guid);
  }
  g_done.notify_all();
  return true;
}

bool ReadCachedItem(const std::string& guid, std::vector<uint8_t>& out) {
  std::ifstream in(CachePath(guid), std::ios::binary | std::ios::ate);
  if (!in) return false;
  const std::streamsize size = in.tellg();
  if (size <= 0) return false;
  out.resize(size_t(size));
  in.seekg(0);
  return bool(in.read(reinterpret_cast<char*>(out.data()), size));
}

void WaitForItem(const std::string& guid, int timeout_ms) {
  std::unique_lock lock(g_mutex);
  g_done.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !g_busy.count(guid); });
}

}  // namespace cc2::avatar_fetch
