#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Peer transport through the CC2 UDP relay. Each lobby member gets a pid 1..4 and the fake address 10.66.0.<pid>;
// the game's VDP traffic to those addresses goes to the relay as DATA and comes back the same way.
namespace cc2::relay {

constexpr uint16_t kGamePort = 1000;
constexpr uint8_t kNoPidOctet = 250;  // own address before a lobby join
constexpr size_t kMaxPayload = 1400;

struct Datagram {
  uint8_t src_pid = 0;
  uint16_t port = 0;
  std::vector<uint8_t> data;
};

// relay is "host:port", token the server's relay_token, pid the one the join response assigned.
void Start(const std::string& relay, const std::string& token, int pid);
void Stop();
bool Running();

int Pid();              // 0 until joined
uint8_t Members();      // bit (pid-1) per member bound at the relay
uint8_t AddressOctet(); // last octet of the local 10.66.0.x address

// dst_pid 0 sends to every other member. Returns false when the relay is not up or the payload is out of range.
bool Send(uint8_t dst_pid, uint16_t port, const uint8_t* data, size_t size);
// Pops the oldest received datagram; false when the queue is empty.
bool Pop(Datagram& out);
bool Pending();
// Called on the relay thread whenever a datagram is queued, so pending receives can complete.
void SetReceiveHook(void (*hook)());

// 36-byte XNADDR for a pid (pid 0 gives the pre-join address).
void FillXnAddr(uint8_t pid_octet, uint8_t* xnaddr);
// True for 10.66.0.x, with the last octet in pid_octet. in_addr is the host-order value (0x0A4200xx).
bool IsPeerAddress(uint32_t in_addr, uint8_t* pid_octet = nullptr);

}  // namespace cc2::relay
