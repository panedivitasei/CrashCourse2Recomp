#include "cc2/relay.h"
#include "cc2/lobby.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include <rex/logging.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace cc2::relay {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint8_t kMagic[4] = {'C', 'C', '2', 'R'};
constexpr uint8_t kHello = 1, kData = 2, kHelloAck = 3;
constexpr auto kHelloEvery = std::chrono::seconds(5);
constexpr size_t kQueueCap = 512;

std::mutex g_control;  // Start/Stop
std::thread g_thread;
std::atomic<bool> g_stop{false}, g_running{false}, g_ready{false};
std::atomic<int> g_pid{0};
std::atomic<uint8_t> g_members{0};
std::atomic<void (*)()> g_hook{nullptr};

// The socket and the relay's address; Send runs on guest threads, so both sit behind this lock.
std::mutex g_sock_mutex;
SOCKET g_sock = INVALID_SOCKET;
sockaddr_in g_relay_addr{};

std::mutex g_queue_mutex;
std::deque<Datagram> g_queue;

std::atomic<bool> g_logged_first_send{false}, g_logged_first_recv{false}, g_logged_overflow{false};

bool Resolve(const std::string& relay, sockaddr_in& out) {
  const size_t colon = relay.rfind(':');
  if (colon == std::string::npos) return false;
  const std::string host = relay.substr(0, colon), port = relay.substr(colon + 1);
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0 || !result) return false;
  std::memcpy(&out, result->ai_addr, sizeof(out));
  freeaddrinfo(result);
  return true;
}

void SendRaw(const uint8_t* data, size_t size) {
  std::lock_guard lock(g_sock_mutex);
  if (g_sock == INVALID_SOCKET) return;
  sendto(g_sock, reinterpret_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&g_relay_addr),
         sizeof(g_relay_addr));
}

void SendHello(const std::string& token) {
  std::vector<uint8_t> packet(kMagic, kMagic + 4);
  packet.push_back(kHello);
  packet.insert(packet.end(), token.begin(), token.end());
  SendRaw(packet.data(), packet.size());
}

void Queue(uint8_t src, uint16_t port, const uint8_t* data, size_t size) {
  {
    std::lock_guard lock(g_queue_mutex);
    if (g_queue.size() >= kQueueCap) {
      g_queue.pop_front();
      if (!g_logged_overflow.exchange(true)) {
        REXLOG_WARN("[relay] receive queue full; dropping the oldest datagrams (the game is not reading)");
      }
    }
    g_queue.push_back(Datagram{src, port, std::vector<uint8_t>(data, data + size)});
  }
  if (auto hook = g_hook.load()) hook();
}

void Worker(std::string relay, std::string token, int expected_pid) {
  sockaddr_in addr{};
  while (!g_stop && !Resolve(relay, addr)) {
    REXLOG_WARN("[relay] can't resolve {}; retrying in 5 s", relay);
    for (int i = 0; i < 20 && !g_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  if (g_stop) return;
  SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) {
    REXLOG_ERROR("[relay] socket() failed ({})", WSAGetLastError());
    return;
  }
  {
    std::lock_guard lock(g_sock_mutex);
    g_sock = sock;
    g_relay_addr = addr;
  }
  char text[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &addr.sin_addr, text, sizeof(text));
  REXLOG_INFO("[relay] talking to {} ({}:{}) as pid {}", relay, text, ntohs(addr.sin_port), expected_pid);

  const auto started = Clock::now();
  auto next_hello = started;
  bool acked = false, warned_silent = false;
  uint8_t buffer[2048];
  while (!g_stop) {
    const auto now = Clock::now();
    if (now >= next_hello) {
      SendHello(token);
      next_hello = now + kHelloEvery;
    }
    if (!acked && !warned_silent && now - started > std::chrono::seconds(15)) {
      warned_silent = true;
      REXLOG_WARN("[relay] no HELLO_ACK after 15 s; the relay may be down or the token rejected");
    }
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(sock, &read_set);
    timeval timeout{0, 250000};
    if (select(0, &read_set, nullptr, nullptr, &timeout) <= 0) continue;
    sockaddr_in from{};
    int from_len = sizeof(from);
    const int got = recvfrom(sock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
    if (got < 5 || std::memcmp(buffer, kMagic, 4) != 0) continue;
    if (from.sin_addr.s_addr != addr.sin_addr.s_addr || from.sin_port != addr.sin_port) continue;
    const uint8_t type = buffer[4];
    if (type == kHelloAck && got >= 7) {
      const int pid = buffer[5];
      const uint8_t members = buffer[6];
      if (!acked) {
        acked = true;
        g_ready = true;
        REXLOG_INFO("[relay] connected: pid {}, members {:#04x}", pid, members);
        if (expected_pid && pid != expected_pid) {
          REXLOG_WARN("[relay] relay says pid {} but the server assigned {}; using the relay's", pid, expected_pid);
        }
      }
      if (pid != g_pid.exchange(pid) && acked) REXLOG_INFO("[relay] pid is now {}", pid);
      const uint8_t previous = g_members.exchange(members);
      if (previous != members) REXLOG_INFO("[relay] members {:#04x} -> {:#04x}", previous, members);
      // A new member means the game will soon read its avatar, so warm the lobby's manifest cache now.
      if (members & ~previous) lobby::RequestPeerRefresh();
    } else if (type == kData && got >= 9) {
      const uint8_t src = buffer[5];
      const uint16_t port = uint16_t((buffer[6] << 8) | buffer[7]);
      Queue(src, port, buffer + 8, size_t(got - 8));
    }
  }
}

}  // namespace

void Start(const std::string& relay, const std::string& token, int pid) {
  Stop();
  std::lock_guard lock(g_control);
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  g_stop = false;
  g_ready = false;
  g_pid = pid;
  g_members = 0;
  g_logged_first_send = false;
  g_logged_first_recv = false;
  g_logged_overflow = false;
  {
    std::lock_guard queue_lock(g_queue_mutex);
    g_queue.clear();
  }
  g_running = true;
  g_thread = std::thread(Worker, relay, token, pid);
}

void Stop() {
  std::lock_guard lock(g_control);
  if (!g_thread.joinable()) return;
  g_stop = true;
  g_thread.join();
  {
    std::lock_guard sock_lock(g_sock_mutex);
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    g_sock = INVALID_SOCKET;
  }
  {
    std::lock_guard queue_lock(g_queue_mutex);
    g_queue.clear();
  }
  g_running = false;
  g_ready = false;
  g_pid = 0;
  g_members = 0;
  WSACleanup();
  REXLOG_INFO("[relay] stopped");
}

bool Running() { return g_running.load(); }
int Pid() { return g_pid.load(); }
uint8_t Members() { return g_members.load(); }
uint8_t AddressOctet() {
  const int pid = g_pid.load();
  return pid > 0 && pid < 250 ? uint8_t(pid) : kNoPidOctet;
}

bool Send(uint8_t dst_pid, uint16_t port, const uint8_t* data, size_t size) {
  if (!g_ready.load() || !size || size > kMaxPayload) return false;
  uint8_t packet[8 + kMaxPayload];
  std::memcpy(packet, kMagic, 4);
  packet[4] = kData;
  packet[5] = dst_pid;
  packet[6] = uint8_t(port >> 8);
  packet[7] = uint8_t(port);
  std::memcpy(packet + 8, data, size);
  SendRaw(packet, 8 + size);
  return true;
}

bool Pop(Datagram& out) {
  std::lock_guard lock(g_queue_mutex);
  if (g_queue.empty()) return false;
  out = std::move(g_queue.front());
  g_queue.pop_front();
  return true;
}

bool Pending() {
  std::lock_guard lock(g_queue_mutex);
  return !g_queue.empty();
}

void SetReceiveHook(void (*hook)()) { g_hook.store(hook); }

void FillXnAddr(uint8_t pid_octet, uint8_t* xnaddr) {
  const uint8_t octet = pid_octet ? pid_octet : kNoPidOctet;
  const uint8_t ina[4] = {10, 66, 0, octet};
  std::memset(xnaddr, 0, 36);
  std::memcpy(xnaddr + 0, ina, 4);  // ina
  std::memcpy(xnaddr + 4, ina, 4);  // inaOnline
  xnaddr[8] = uint8_t(kGamePort >> 8);  // wPortOnline, big-endian
  xnaddr[9] = uint8_t(kGamePort & 0xFF);
  const uint8_t enet[6] = {0x02, 0xCC, 0x2A, 0x00, 0x00, octet};
  std::memcpy(xnaddr + 10, enet, 6);
  std::memcpy(xnaddr + 16, ina, 4);  // abOnline: ina then zeros
}

bool IsPeerAddress(uint32_t in_addr, uint8_t* pid_octet) {
  if ((in_addr & 0xFFFFFF00u) != 0x0A420000u) return false;
  if (pid_octet) *pid_octet = uint8_t(in_addr & 0xFF);
  return true;
}

}  // namespace cc2::relay
