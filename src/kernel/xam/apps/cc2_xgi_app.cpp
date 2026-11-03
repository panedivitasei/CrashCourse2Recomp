// See cc2_xgi_app.h.

#include "cc2/kernel/xam/apps/cc2_xgi_app.h"

#include <algorithm>
#include <cstring>

#include "cc2/kernel/xnet.h"
#include "cc2/lobby.h"
#include <rex/logging.h>
#include <rex/memory.h>

namespace cc2::kernel::xam::apps {

namespace {
constexpr uint32_t kXUserReadStats = 0x000B0021;
constexpr uint32_t kXSessionCreate = 0x000B0010;
constexpr uint32_t kXSessionDelete = 0x000B0011;
constexpr uint32_t kXSessionJoin = 0x000B0012;
constexpr uint32_t kXSessionLeave = 0x000B0013;
constexpr uint32_t kXSessionStart = 0x000B0014;
constexpr uint32_t kXSessionEnd = 0x000B0015;
constexpr uint32_t kXSessionModify = 0x000B0018;
constexpr uint32_t kXSessionSearchById = 0x000B001B;
constexpr uint32_t kXSessionSearchEx = 0x000B001C;
constexpr uint32_t kXSessionMigrateHost = 0x000B001E;
constexpr uint32_t kXUserMaxCount = 4;  // a user index below this makes the local console the new host
constexpr uint8_t kXnKidOnlinePeer = 0x80;  // XNET_XNKID_ONLINE_PEER in the id's top nibble
}  // namespace

// The session layer for hosting: XAM would register the session with Live; here the XAM side only has to hand
// back what a host console gets (session id, host address, key, nonce) and accept the follow-up calls.
X_HRESULT Cc2XgiApp::DispatchSessionMessage(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  auto* buffer = memory_->TranslateVirtual(buffer_ptr);
  auto u32 = [&](uint32_t offset) { return rex::memory::load_and_swap<uint32_t>(buffer + offset); };
  switch (message) {
    case kXSessionCreate: {
      const uint32_t flags = u32(4), user = u32(16), info_ptr = u32(20), nonce_ptr = u32(24);
      const uint32_t public_slots = u32(8), private_slots = u32(12);
      XSESSION_INFO info{};
      cc2::lobby::RandomBytes(info.sessionID.ab, sizeof(info.sessionID.ab));
      info.sessionID.ab[0] = uint8_t((info.sessionID.ab[0] & 0x0F) | kXnKidOnlinePeer);
      cc2::lobby::FillLocalXnAddr(reinterpret_cast<uint8_t*>(&info.hostAddress));
      cc2::lobby::RandomBytes(info.keyExchangeKey.ab, sizeof(info.keyExchangeKey.ab));
      // Joining someone else's session passes their info in; only a host gets a fresh one.
      const bool host = (flags & 0x1) != 0;  // XSESSION_CREATE_HOST
      if (info_ptr && host) std::memcpy(memory_->TranslateVirtual(info_ptr), &info, sizeof(info));
      if (info_ptr && !host) std::memcpy(&info, memory_->TranslateVirtual(info_ptr), sizeof(info));
      uint64_t nonce = 0;
      cc2::lobby::RandomBytes(&nonce, sizeof(nonce));
      if (nonce_ptr) rex::memory::store_and_swap<uint64_t>(memory_->TranslateVirtual(nonce_ptr), nonce);
      REXKRNL_INFO("[lobby] XSessionCreate(flags={:08X}, user={}, public={}, private={}) {} session {:016X}",
                   flags, user, public_slots, private_slots, host ? "hosts" : "joins",
                   rex::byte_swap(*reinterpret_cast<const uint64_t*>(info.sessionID.ab)));
      cc2::lobby::OnSessionCreated(host, reinterpret_cast<const uint8_t*>(&info));
      return X_E_SUCCESS;
    }
    case kXSessionDelete:
      REXKRNL_INFO("[lobby] XSessionDelete");
      cc2::lobby::OnSessionDeleted();
      return X_E_SUCCESS;
    case kXSessionJoin:
      REXKRNL_INFO("[lobby] XSessionJoin{}({} users)", u32(8) ? "Remote" : "Local", u32(4));
      return X_E_SUCCESS;
    case kXSessionLeave:
      REXKRNL_INFO("[lobby] XSessionLeave{}({} users)", u32(8) ? "Remote" : "Local", u32(4));
      return X_E_SUCCESS;
    case kXSessionStart:
      REXKRNL_INFO("[lobby] XSessionStart");
      return X_E_SUCCESS;
    case kXSessionEnd:
      REXKRNL_INFO("[lobby] XSessionEnd");
      return X_E_SUCCESS;
    case kXSessionModify:
      REXKRNL_INFO("[lobby] XSessionModify(flags={:08X}, public={}, private={})", u32(4), u32(8), u32(12));
      return X_E_SUCCESS;
    case kXSessionSearchEx:
    case kXSessionSearchById: {
      // A lobby member's search finds exactly the host's published session; otherwise the zeroed header the
      // title's wrapper left reads as zero sessions found.
      const bool ex = message == kXSessionSearchEx;
      const uint32_t size = ex ? u32(24) : u32(12), results_ptr = ex ? u32(28) : u32(16);
      const char* name = ex ? "SearchEx" : "SearchByID";
      XSESSION_INFO target{};
      if (!results_ptr || size < sizeof(XSESSION_SEARCHRESULT_HEADER) + sizeof(XSESSION_SEARCHRESULT) ||
          !cc2::lobby::JoinTarget(reinterpret_cast<uint8_t*>(&target))) {
        REXKRNL_INFO("[lobby] XSession{} -> 0 results", name);
        return X_E_SUCCESS;
      }
      auto* header = memory_->TranslateVirtual<XSESSION_SEARCHRESULT_HEADER*>(results_ptr);
      const uint32_t result_ptr = results_ptr + uint32_t(sizeof(XSESSION_SEARCHRESULT_HEADER));
      auto* result = memory_->TranslateVirtual<XSESSION_SEARCHRESULT*>(result_ptr);
      std::memset(result, 0, sizeof(*result));
      result->info = target;
      result->open_public_slots = 3;
      result->filled_public_slots = 1;
      const auto* host_ip = reinterpret_cast<const uint8_t*>(&target.hostAddress);
      header->search_results_count = 1;
      header->search_results_ptr = result_ptr;
      REXKRNL_INFO("[lobby] XSession{} -> the lobby host's session {:016X} at {}.{}.{}.{}", name,
                   rex::byte_swap(*reinterpret_cast<const uint64_t*>(target.sessionID.ab)),
                   host_ip[0], host_ip[1], host_ip[2], host_ip[3]);
      return X_E_SUCCESS;
    }
    case kXSessionMigrateHost: {
      // Buffer: session object, XSESSION_INFO*, user index. XUSER_INDEX_NONE means another peer hosts and the game
      // passes that host's info in; a real user index means this console hosts and XAM fills the info.
      const uint32_t info_ptr = u32(4), user = u32(8);
      if (!info_ptr) {
        REXKRNL_WARN("[lobby] XSessionMigrateHost(user={}) without session info", user);
        return X_E_SUCCESS;
      }
      auto* guest_info = memory_->TranslateVirtual(info_ptr);
      XSESSION_INFO info{};
      std::memcpy(&info, guest_info, sizeof(info));
      uint8_t local_addr[sizeof(info.hostAddress)] = {};
      cc2::lobby::FillLocalXnAddr(local_addr);
      const auto* incoming_ip = reinterpret_cast<const uint8_t*>(&info.hostAddress);
      const bool local = user < kXUserMaxCount || std::memcmp(incoming_ip, local_addr, 4) == 0;
      if (local) {
        // Same session id and key as the session being migrated, with this console as the host address.
        XSESSION_INFO current{};
        if (cc2::lobby::CurrentSessionInfo(reinterpret_cast<uint8_t*>(&current))) {
          info.sessionID = current.sessionID;
          info.keyExchangeKey = current.keyExchangeKey;
        }
        std::memcpy(&info.hostAddress, local_addr, sizeof(local_addr));
        std::memcpy(guest_info, &info, sizeof(info));
        REXKRNL_INFO("[lobby] XSessionMigrateHost(user={}) -> this machine hosts session {:016X}", user,
                     rex::byte_swap(*reinterpret_cast<const uint64_t*>(info.sessionID.ab)));
      } else {
        REXKRNL_INFO("[lobby] XSessionMigrateHost(user={}) -> new host pid {} ({}.{}.{}.{}), session {:016X}", user,
                     incoming_ip[3], incoming_ip[0], incoming_ip[1], incoming_ip[2], incoming_ip[3],
                     rex::byte_swap(*reinterpret_cast<const uint64_t*>(info.sessionID.ab)));
      }
      cc2::lobby::OnHostMigrated(local, reinterpret_cast<const uint8_t*>(&info));
      return X_E_SUCCESS;
    }
    default:
      return XgiApp::DispatchMessageSync(message, buffer_ptr, buffer_length);
  }
}

X_HRESULT Cc2XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                         uint32_t buffer_length) {
  if (message >= kXSessionCreate && message <= kXSessionMigrateHost && buffer_ptr) {
    return DispatchSessionMessage(message, buffer_ptr, buffer_length);
  }
  if (message != kXUserReadStats) {
    return XgiApp::DispatchMessageSync(message, buffer_ptr, buffer_length);
  }
  auto* buffer = memory_->TranslateVirtual(buffer_ptr);
  const uint32_t specs_count = rex::memory::load_and_swap<uint32_t>(buffer + 12);
  const uint32_t specs_ptr = rex::memory::load_and_swap<uint32_t>(buffer + 16);
  const uint32_t results_size = rex::memory::load_and_swap<uint32_t>(buffer + 20);
  const uint32_t results_ptr = rex::memory::load_and_swap<uint32_t>(buffer + 24);
  REXKRNL_DEBUG("XUserReadStats(specs={} @{:08X}, results {} bytes @{:08X})", specs_count,
                specs_ptr, results_size, results_ptr);
  if (!results_ptr || results_size < sizeof(X_USER_STATS_READ_RESULTS)) {
    return X_E_INVALIDARG;
  }

  // One zero-row view per requested spec, rows_ptr kept valid (titles
  // dereference it even with zero rows) by pointing it at the zeroed tail.
  uint8_t* results = memory_->TranslateVirtual(results_ptr);
  std::memset(results, 0, results_size);
  const uint32_t view_capacity = static_cast<uint32_t>(
      (results_size - sizeof(X_USER_STATS_READ_RESULTS)) / sizeof(X_USER_STATS_VIEW));
  const uint32_t num_views = std::min(specs_count, view_capacity);
  const uint32_t views_guest = results_ptr + static_cast<uint32_t>(sizeof(X_USER_STATS_READ_RESULTS));

  auto* header = reinterpret_cast<X_USER_STATS_READ_RESULTS*>(results);
  header->num_views = num_views;
  header->views_ptr = views_guest;

  auto* views = reinterpret_cast<X_USER_STATS_VIEW*>(results + sizeof(X_USER_STATS_READ_RESULTS));
  uint32_t rows_guest = views_guest + num_views * static_cast<uint32_t>(sizeof(X_USER_STATS_VIEW));
  if (rows_guest >= results_ptr + results_size) {
    rows_guest = views_guest;
  }
  const X_USER_STATS_SPEC* specs =
      specs_ptr ? memory_->TranslateVirtual<const X_USER_STATS_SPEC*>(specs_ptr) : nullptr;
  for (uint32_t i = 0; i < num_views; ++i) {
    views[i].view_id = specs ? uint32_t(specs[i].view_id) : 0;
    views[i].total_view_rows = 0;
    views[i].num_rows = 0;
    views[i].rows_ptr = rows_guest;
  }
  return X_E_SUCCESS;
}

}  // namespace cc2::kernel::xam::apps
