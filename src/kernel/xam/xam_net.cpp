// Network status and the XNet pieces a session needs, title-side. The runtime's no-link, no-machine-id answers read as
// a latched "offline", so this reports a live 100 Mbps link; peer traffic on the VDP port goes through cc2::relay.

#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/types.h>

#include "cc2/lobby.h"
#include "cc2/online.h"
#include "cc2/relay.h"

// Last, as the runtime headers have pulled in windows.h by now.
#include <winsock2.h>

namespace rex::kernel::xam {

namespace {
constexpr uint32_t kLinkActive = 0x01;
constexpr uint32_t kLink100Mbps = 0x02;
constexpr uint32_t kLinkFullDuplex = 0x08;
constexpr uint32_t kMachineIdHigh = 0xFA00BABE;  // machine ids are 0xFA-prefixed

constexpr uint32_t kXnAddrStatic = 0x04, kXnAddrGateway = 0x20, kXnAddrDns = 0x40, kXnAddrOnline = 0x80;
constexpr uint32_t kIoPending = 997, kIoIncomplete = 996, kOperationAborted = 995;
constexpr uint32_t kWouldBlock = 10035, kNotSocket = 10038, kMsgSize = 10040, kInvalid = 10022;
constexpr uint32_t kConnectStatusConnected = 2, kConnectStatusLost = 3;
constexpr uint8_t kQosComplete = 0x01, kQosContacted = 0x02;

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;       // our completion code, kIoPending while outstanding
  rex::be<uint32_t> internal_high;  // bytes transferred
  rex::be<uint32_t> offset;
  rex::be<uint32_t> offset_high;
  rex::be<uint32_t> event_handle;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XQOSINFO {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min;
  rex::be<uint16_t> rtt_med;
  rex::be<uint32_t> up_bits;
  rex::be<uint32_t> down_bits;
};
static_assert(sizeof(XQOSINFO) == 24);

template <typename T>
T* Guest(uint32_t address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<T*>(address);
}

// A receive the game keeps posted on a socket; closing the socket aborts it. Numbers-of-bytes and flags pointers
// live on the caller's stack, so a late completion only touches the buffers, the from address and the overlapped.
struct PendingRecv {
  uint32_t overlapped = 0;
  uint32_t from = 0, from_len = 0;
  std::vector<std::pair<uint32_t, uint32_t>> buffers;  // guest pointer, length
};

std::mutex g_recv_mutex;
std::unordered_map<uint32_t, PendingRecv> g_pending_recv;
std::unordered_map<uint32_t, uint16_t> g_relayed;  // socket handle -> game port it bound, for sockets on the VDP port
std::atomic<bool> g_logged_drop{false}, g_logged_rebind{false};

void SignalOverlapped(uint32_t overlapped_ptr, uint32_t code, uint32_t bytes) {
  auto* o = Guest<XWSAOVERLAPPED>(overlapped_ptr);
  o->internal = code;
  o->internal_high = bytes;
  if (auto ev = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(o->event_handle)) ev->Set(0, false);
}

void WriteFrom(uint32_t from_ptr, uint32_t from_len_ptr, const cc2::relay::Datagram& d) {
  if (from_ptr) {
    auto* from = Guest<rex::system::XSOCKADDR_IN>(from_ptr);
    from->sin_family = 2;
    from->sin_port = d.port;
    from->sin_addr = 0x0A420000u | d.src_pid;
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }
  if (from_len_ptr) *Guest<rex::be<uint32_t>>(from_len_ptr) = 16;
}

// Copies a datagram into the guest buffers; returns the bytes copied and whether it was cut short.
uint32_t CopyOut(const std::vector<std::pair<uint32_t, uint32_t>>& buffers, const cc2::relay::Datagram& d,
                 bool& truncated) {
  size_t offset = 0;
  for (const auto& [ptr, len] : buffers) {
    if (offset >= d.data.size()) break;
    const size_t n = std::min<size_t>(len, d.data.size() - offset);
    if (ptr && n) std::memcpy(Guest<uint8_t>(ptr), d.data.data() + offset, n);
    offset += n;
  }
  truncated = offset < d.data.size();
  return uint32_t(offset);
}

bool IsRelayed(uint32_t socket_handle, uint16_t* port = nullptr) {
  auto it = g_relayed.find(socket_handle);
  if (it == g_relayed.end()) return false;
  if (port) *port = it->second;
  return true;
}

// Matches queued relay datagrams with posted receives. Runs on guest threads and on the relay thread.
void Deliver() {
  std::lock_guard lock(g_recv_mutex);
  while (true) {
    uint32_t any = 0;
    for (const auto& [handle, pending] : g_pending_recv) {
      if (g_relayed.count(handle)) {
        any = handle;
        break;
      }
    }
    if (!any) return;
    cc2::relay::Datagram d;
    if (!cc2::relay::Pop(d)) return;
    uint32_t target = any;
    for (const auto& [handle, pending] : g_pending_recv) {
      uint16_t port = 0;
      if (IsRelayed(handle, &port) && port == d.port) {
        target = handle;
        break;
      }
    }
    PendingRecv pending = std::move(g_pending_recv[target]);
    g_pending_recv.erase(target);
    bool truncated = false;
    const uint32_t bytes = CopyOut(pending.buffers, d, truncated);
    WriteFrom(pending.from, pending.from_len, d);
    SignalOverlapped(pending.overlapped, truncated ? kMsgSize : 0, bytes);
  }
}

struct HookInstaller {
  HookInstaller() { cc2::relay::SetReceiveHook(&Deliver); }
} g_hook_installer;

// Relay routing for a send on a VDP-port socket; false means the datagram was dropped.
bool RouteSend(uint32_t to_ptr, uint16_t fallback_port, const uint8_t* data, size_t size) {
  uint32_t addr = 0xFFFFFFFFu;
  uint16_t port = fallback_port;
  if (to_ptr) {
    auto* to = Guest<rex::system::XSOCKADDR_IN>(to_ptr);
    addr = to->sin_addr;
    port = to->sin_port;
  }
  uint8_t octet = 0;
  uint8_t dst = 0;
  if (addr == 0xFFFFFFFFu) {
    dst = 0;
  } else if (cc2::relay::IsPeerAddress(addr, &octet) && octet >= 1 && octet <= 4) {
    dst = octet;
  } else if (cc2::relay::IsPeerAddress(addr, &octet) && octet == 255) {
    dst = 0;
  } else {
    if (!g_logged_drop.exchange(true)) {
      REXLOG_WARN("[relay] dropping game datagram to {}.{}.{}.{}:{} (not a lobby address)", addr >> 24,
                  (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF, port);
    }
    return false;
  }
  return cc2::relay::Send(dst, port, data, size);
}

std::vector<uint8_t> Gather(uint32_t buffers_ptr, uint32_t count) {
  std::vector<uint8_t> out;
  auto* buffers = Guest<XWSABUF>(buffers_ptr);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t len = buffers[i].len, ptr = buffers[i].buf_ptr;
    if (!len || !ptr) continue;
    const uint8_t* src = Guest<uint8_t>(ptr);
    out.insert(out.end(), src, src + len);
  }
  return out;
}
}  // namespace

// Session keys and ids come from here, so two consoles must not produce the same bytes.
u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  if (buffer_ptr && length) cc2::lobby::RandomBytes(buffer_ptr, length);
  return 0;
}

// Every player's address is its lobby pid's 10.66.0.x, which the relay routes.
u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, mapped_void addr_ptr) {
  if (addr_ptr) cc2::lobby::FillLocalXnAddr(static_cast<uint8_t*>(static_cast<void*>(addr_ptr)));
  return kXnAddrStatic | kXnAddrGateway | kXnAddrDns | (cc2::online::ReportLive() ? kXnAddrOnline : 0);
}

u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, u32 xnaddr_ptr, u32 xnkid_ptr, u32 in_addr_ptr) {
  if (!xnaddr_ptr || !in_addr_ptr) return kInvalid;
  std::memcpy(Guest<uint8_t>(in_addr_ptr), Guest<uint8_t>(xnaddr_ptr), 4);
  return 0;
}

u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, u32 in_addr, u32 xnaddr_ptr, u32 xnkid_ptr) {
  uint8_t octet = 0;
  if (!cc2::relay::IsPeerAddress(in_addr, &octet)) return kInvalid;
  if (xnaddr_ptr) cc2::relay::FillXnAddr(octet, Guest<uint8_t>(xnaddr_ptr));
  if (xnkid_ptr) cc2::lobby::CurrentSessionId(Guest<uint8_t>(xnkid_ptr));
  return 0;
}

// The relay stands in for secure associations: every lobby address counts as connected.
u32 NetDll_XNetConnect_entry(u32 caller, u32 in_addr) { return 0; }
u32 NetDll_XNetGetConnectStatus_entry(u32 caller, u32 in_addr) {
  return cc2::relay::IsPeerAddress(in_addr) ? kConnectStatusConnected : kConnectStatusLost;
}

// QoS answers locally: one completed probe per target at 40 ms, since the relay is the only path anyway.
u32 NetDll_XNetQosLookup_entry(u32 caller, u32 xna_count, u32 xna_ptrs, u32 xnkid_ptrs, u32 xnkey_ptrs,
                               u32 ina_count, u32 ina_ptr, u32 service_ids, u32 probes, u32 bits_per_sec, u32 flags,
                               u32 event_handle, u32 qos_out_ptr) {
  const uint32_t count = xna_count + ina_count;
  if (!qos_out_ptr) return kInvalid;
  const uint32_t size = 8 + 24 * std::max<uint32_t>(count, 1);
  const uint32_t qos = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  if (!qos) return 8;  // ERROR_NOT_ENOUGH_MEMORY
  std::memset(Guest<uint8_t>(qos), 0, size);
  *Guest<rex::be<uint32_t>>(qos) = count;
  *Guest<rex::be<uint32_t>>(qos + 4) = 0;
  auto* info = Guest<XQOSINFO>(qos + 8);
  const uint16_t sent = uint16_t(std::clamp<uint32_t>(probes, 1, 16));
  for (uint32_t i = 0; i < count; ++i) {
    info[i].flags = kQosComplete | kQosContacted;
    info[i].probes_xmit = sent;
    info[i].probes_recv = sent;
    info[i].rtt_min = 40;
    info[i].rtt_med = 40;
    info[i].up_bits = 1000000;
    info[i].down_bits = 1000000;
  }
  *Guest<rex::be<uint32_t>>(qos_out_ptr) = qos;
  if (event_handle) {
    if (auto ev = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(event_handle)) ev->Set(0, false);
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) REXLOG_INFO("[relay] QoS lookup answered locally for {} targets", count);
  return 0;
}

// A host registers its QoS reply data here; lookups are answered locally, so accepting it is enough.
u32 NetDll_XNetQosListen_entry(u32 caller, mapped_void id, mapped_void data, u32 data_size, u32 r7,
                               u32 flags) {
  return 0;
}

// Security associations are the relay's job; the key calls succeed so session setup can continue.
u32 NetDll_XNetRegisterKey_entry(u32 caller, mapped_void xnkid, mapped_void xnkey) { return 0; }
u32 NetDll_XNetUnregisterKey_entry(u32 caller, mapped_void xnkid) { return 0; }
u32 NetDll_XNetReplaceKey_entry(u32 caller, mapped_void old_xnkid, mapped_void new_xnkid) { return 0; }
u32 NetDll_XNetUnregisterInAddr_entry(u32 caller, u32 in_addr) { return 0; }

// Sockets bound to the VDP port are relayed. A second instance on the same PC can't take the host port, so it binds
// an ephemeral one; the game only ever sees the port it asked for.
u32 NetDll_bind_entry(u32 caller, u32 socket_handle, u32 name_ptr, u32 name_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(socket_handle);
  if (!socket) {
    rex::system::XThread::SetLastError(kNotSocket);
    return uint32_t(-1);
  }
  rex::system::N_XSOCKADDR_IN native(Guest<rex::system::XSOCKADDR_IN>(name_ptr));
  const uint16_t port = native.sin_port;
  X_STATUS status = socket->Bind(&native, name_len);
  if (XFAILED(status) && port == cc2::relay::kGamePort) {
    native.sin_port = 0;
    status = socket->Bind(&native, name_len);
    if (XSUCCEEDED(status) && !g_logged_rebind.exchange(true)) {
      REXLOG_INFO("[relay] host port {} is taken; the VDP socket uses an ephemeral port (traffic goes via the relay)",
                  port);
    }
  }
  if (XFAILED(status)) {
    rex::system::XThread::SetLastError(10048);  // WSAEADDRINUSE
    return uint32_t(-1);
  }
  if (port == cc2::relay::kGamePort) {
    std::lock_guard lock(g_recv_mutex);
    g_relayed[socket_handle] = port;
    REXLOG_INFO("[relay] game socket {:08X} bound to port {}; its traffic is relayed", socket_handle, port);
  }
  return 0;
}

// XRNM keeps one overlapped receive posted per VDP socket and fails the session if posting it fails. The argument
// order is the XDK's: from, fromlen, then overlapped and the completion routine on the stack.
u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket_handle, u32 buffers_ptr, u32 buffer_count, u32 num_bytes_ptr,
                             u32 flags_ptr, u32 from_ptr, u32 from_len_ptr, u32 overlapped_ptr,
                             u32 completion_routine_ptr) {
  if (!REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(socket_handle)) {
    rex::system::XThread::SetLastError(kNotSocket);
    return uint32_t(-1);
  }
  PendingRecv pending;
  pending.overlapped = overlapped_ptr;
  pending.from = from_ptr;
  pending.from_len = from_len_ptr;
  auto* buffers = Guest<XWSABUF>(buffers_ptr);
  for (uint32_t i = 0; buffers_ptr && i < buffer_count; ++i) {
    pending.buffers.emplace_back(uint32_t(buffers[i].buf_ptr), uint32_t(buffers[i].len));
  }
  bool relayed;
  {
    std::lock_guard lock(g_recv_mutex);
    relayed = IsRelayed(socket_handle);
  }
  if (!overlapped_ptr) {
    cc2::relay::Datagram d;
    if (!relayed || !cc2::relay::Pop(d)) {
      rex::system::XThread::SetLastError(kWouldBlock);
      return uint32_t(-1);
    }
    bool truncated = false;
    const uint32_t bytes = CopyOut(pending.buffers, d, truncated);
    WriteFrom(from_ptr, from_len_ptr, d);
    if (num_bytes_ptr) *Guest<rex::be<uint32_t>>(num_bytes_ptr) = bytes;
    if (flags_ptr) *Guest<rex::be<uint32_t>>(flags_ptr) = 0;
    if (truncated) {
      rex::system::XThread::SetLastError(kMsgSize);
      return uint32_t(-1);
    }
    return 0;
  }
  auto* o = Guest<XWSAOVERLAPPED>(overlapped_ptr);
  o->internal = kIoPending;
  o->internal_high = 0;
  {
    std::lock_guard lock(g_recv_mutex);
    g_pending_recv[socket_handle] = std::move(pending);
  }
  // A datagram already queued completes the receive at once, event and all, as Winsock does.
  if (relayed) Deliver();
  rex::system::XThread::SetLastError(kIoPending);
  return uint32_t(-1);
}

u32 NetDll_WSAGetOverlappedResult_entry(u32 caller, u32 socket_handle, mapped_void overlapped_ptr,
                                        mapped_u32 bytes_ptr, u32 wait, mapped_u32 flags_ptr) {
  if (!overlapped_ptr) {
    rex::system::XThread::SetLastError(kIoIncomplete);
    return 0;
  }
  auto* o = static_cast<XWSAOVERLAPPED*>(static_cast<void*>(overlapped_ptr));
  const uint32_t code = o->internal;
  if (code == kIoPending) {
    rex::system::XThread::SetLastError(kIoIncomplete);
    return 0;
  }
  if (bytes_ptr) *bytes_ptr = uint32_t(o->internal_high);
  if (flags_ptr) *flags_ptr = 0;
  if (code != 0) {
    rex::system::XThread::SetLastError(code);
    return 0;
  }
  return 1;
}

// VDP sends arrive as a length/header buffer plus the game data; the relay carries their concatenation unchanged.
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, u32 buffers_ptr, u32 buffer_count, u32 num_bytes_ptr,
                           u32 flags, u32 to_ptr, u32 to_len, u32 overlapped_ptr, u32 completion_routine_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(socket_handle);
  if (!socket) {
    rex::system::XThread::SetLastError(kNotSocket);
    return uint32_t(-1);
  }
  std::vector<uint8_t> data = Gather(buffers_ptr, buffer_count);
  uint16_t port = 0;
  bool relayed;
  {
    std::lock_guard lock(g_recv_mutex);
    relayed = IsRelayed(socket_handle, &port);
  }
  if (relayed) {
    RouteSend(to_ptr, port, data.data(), data.size());
  } else if (to_ptr) {
    rex::system::N_XSOCKADDR_IN native_to(Guest<rex::system::XSOCKADDR_IN>(to_ptr));
    socket->SendTo(data.data(), uint32_t(data.size()), flags, &native_to, to_len);
  }
  // UDP has no delivery report, so a dropped datagram still reads as sent.
  if (num_bytes_ptr) *Guest<rex::be<uint32_t>>(num_bytes_ptr) = uint32_t(data.size());
  return 0;
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, u32 buf_ptr, u32 buf_len, u32 flags, u32 to_ptr, u32 to_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(socket_handle);
  if (!socket) {
    rex::system::XThread::SetLastError(kNotSocket);
    return uint32_t(-1);
  }
  uint16_t port = 0;
  bool relayed;
  {
    std::lock_guard lock(g_recv_mutex);
    relayed = IsRelayed(socket_handle, &port);
  }
  if (relayed) {
    RouteSend(to_ptr, port, buf_ptr ? Guest<uint8_t>(buf_ptr) : nullptr, buf_ptr ? buf_len : 0);
    return buf_len;
  }
  rex::system::N_XSOCKADDR_IN native_to(Guest<rex::system::XSOCKADDR_IN>(to_ptr));
  return socket->SendTo(Guest<uint8_t>(buf_ptr), buf_len, flags, &native_to, to_len);
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, u32 buf_ptr, u32 buf_len, u32 flags, u32 from_ptr,
                          u32 from_len_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(socket_handle);
  if (!socket) {
    rex::system::XThread::SetLastError(kNotSocket);
    return uint32_t(-1);
  }
  bool relayed;
  {
    std::lock_guard lock(g_recv_mutex);
    relayed = IsRelayed(socket_handle);
  }
  if (relayed) {
    cc2::relay::Datagram d;
    if (!cc2::relay::Pop(d)) {
      rex::system::XThread::SetLastError(kWouldBlock);
      return uint32_t(-1);
    }
    bool truncated = false;
    const uint32_t bytes = CopyOut({{buf_ptr, buf_len}}, d, truncated);
    WriteFrom(from_ptr, from_len_ptr, d);
    if (truncated) {
      rex::system::XThread::SetLastError(kMsgSize);
      return uint32_t(-1);
    }
    return bytes;
  }
  rex::system::N_XSOCKADDR_IN native_from;
  if (from_ptr) native_from = *Guest<rex::system::XSOCKADDR_IN>(from_ptr);
  uint32_t native_len = from_len_ptr ? uint32_t(*Guest<rex::be<uint32_t>>(from_len_ptr)) : 0;
  const int ret = socket->RecvFrom(Guest<uint8_t>(buf_ptr), buf_len, flags, &native_from,
                                   from_len_ptr ? &native_len : nullptr);
  if (from_ptr) {
    auto* from = Guest<rex::system::XSOCKADDR_IN>(from_ptr);
    from->sin_family = native_from.sin_family;
    from->sin_port = native_from.sin_port;
    from->sin_addr = native_from.sin_addr;
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }
  if (from_len_ptr) *Guest<rex::be<uint32_t>>(from_len_ptr) = native_len;
  if (ret == -1) rex::system::XThread::SetLastError(uint32_t(WSAGetLastError()));
  return uint32_t(ret);
}

// Closing a socket aborts its posted receive, which lets XRNM release the endpoint.
u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  uint32_t pending = 0;
  {
    std::lock_guard lock(g_recv_mutex);
    if (auto it = g_pending_recv.find(socket_handle); it != g_pending_recv.end()) {
      pending = it->second.overlapped;
      g_pending_recv.erase(it);
    }
    if (g_relayed.erase(socket_handle)) REXLOG_INFO("[relay] game socket {:08X} closed", socket_handle);
  }
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(socket_handle);
  if (!socket) {
    rex::system::XThread::SetLastError(kNotSocket);
    return uint32_t(-1);
  }
  socket->Close();
  socket->ReleaseHandle();
  if (pending) SignalOverlapped(pending, kOperationAborted, 0);
  return 0;
}

// QNet tells consoles apart by machine id, so each lobby address gets its own.
u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, u32 addr_ptr, u32 id_ptr) {
  if (id_ptr) {
    const uint8_t octet = addr_ptr ? Guest<uint8_t>(addr_ptr)[3] : 0;
    *Guest<rex::be<uint32_t>>(id_ptr) = kMachineIdHigh;
    *Guest<rex::be<uint32_t>>(id_ptr + 4) = 0xCC2A0000u | octet;
  }
  return 0;
}

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  return kLinkActive | kLink100Mbps | kLinkFullDuplex;
}

}  // namespace rex::kernel::xam

REX_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetConnect, rex::kernel::xam::NetDll_XNetConnect_entry)
REX_EXPORT(__imp__NetDll_XNetGetConnectStatus, rex::kernel::xam::NetDll_XNetGetConnectStatus_entry)
REX_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
REX_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterKey, rex::kernel::xam::NetDll_XNetUnregisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetReplaceKey, rex::kernel::xam::NetDll_XNetReplaceKey_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterInAddr, rex::kernel::xam::NetDll_XNetUnregisterInAddr_entry)
REX_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
REX_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
REX_EXPORT(__imp__NetDll_WSAGetOverlappedResult, rex::kernel::xam::NetDll_WSAGetOverlappedResult_entry)
REX_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
REX_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
REX_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
REX_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
