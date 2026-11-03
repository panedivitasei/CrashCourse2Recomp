// shop_task_guard.cpp: freeze guard for the shop/marketplace task path.
//
// The shop task scheduler can call std::map<int,T>::operator[] on a map that
// was freed and recycled while it still held the pointer. Guest address 0 is
// mapped and reads as zeros, so the descent lands on node 0 with _Isnil 0 and
// both children 0, and loops forever on the main thread where hardware would
// fault.
//
// The hook validates the map at operator[] entry and skips the descent into a
// dead one, logging the trip. It records which map objects were valid, so the
// log tells a use-after-free (address live earlier) from a garbage pointer.

#include <atomic>
#include <mutex>
#include <unordered_set>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

namespace {

// MSVC _Tree_node field offsets (guest, big-endian).
constexpr uint32_t kNodeParent = 4;
constexpr uint32_t kNodeIsNil = 145;
// operator[] returns &node->value, which lives at node+16.
constexpr uint32_t kNodeValue = 16;

uint32_t LoadBE32(const uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr));
}

std::mutex g_mu;
std::unordered_set<uint32_t> g_liveMaps;  // map objects seen with a sane head
std::atomic<uint64_t> g_trips{0};

// Stand-in for the mapped value, allocated once: zeroed except the first
// dword, 10, as the insert path in sub_822D6018 builds a new value ({10, 0...}).
uint32_t EnsureScratchValue() {
  static uint32_t scratch = 0;
  static std::once_flag once;
  std::call_once(once, [] {
    auto* mem = REX_KERNEL_MEMORY();
    const uint32_t block = mem->SystemHeapAlloc(256, 0x20);
    if (!block) {
      REXKRNL_WARN("[shopguard] scratch alloc failed");
      return;
    }
    auto* p = mem->TranslateVirtual<uint8_t*>(block);
    std::memset(p, 0, 256);
    *reinterpret_cast<uint32_t*>(p + kNodeValue) = __builtin_bswap32(10u);
    scratch = block;
  });
  return scratch;
}

}  // namespace

// return_on_true midasm hook at the entry of sub_822E6328 (TU1).
// r3 = the map object, r4 = &key. Returning false runs the original body.
bool DCC2_MapLookupGuard(PPCRegister& r3, PPCRegister& r4) {
  uint8_t* base = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0);
  const uint32_t map_obj = r3.u32;
  const uint32_t key = r4.u32 ? LoadBE32(base, r4.u32) : 0u;

  // A live std::map always has a head node whose _Isnil is set; the head is a
  // real heap allocation, so it is non-null and 4-byte aligned. Recycled
  // memory (typically ASCII) fails these checks.
  bool sane = map_obj >= 0x1000u && map_obj < 0xFFFF0000u;
  uint32_t head = 0;
  uint8_t isnil = 0;
  if (sane) {
    head = LoadBE32(base, map_obj + kNodeParent);
    sane = head >= 0x1000u && head < 0xFFFF0000u && (head & 3u) == 0u;
    if (sane) {
      isnil = *(base + head + kNodeIsNil);
      sane = isnil != 0;
    }
  }

  if (sane) {
    if (g_trips.load(std::memory_order_relaxed) == 0) {
      // The set only grows until the first trip; it exists to classify trips.
      std::lock_guard<std::mutex> lock(g_mu);
      if (g_liveMaps.size() < 4096) g_liveMaps.insert(map_obj);
    }
    return false;  // normal path: run the real lookup
  }

  bool was_live = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    was_live = g_liveMaps.count(map_obj) != 0;
  }
  const uint64_t n = g_trips.fetch_add(1, std::memory_order_relaxed);
  if (n < 24 || (n % 256) == 0) {
    // Dump the object as text: recycled contents identify the new owner.
    char txt[65]{};
    if (map_obj >= 0x1000u && map_obj < 0xFFFF0000u) {
      for (int i = 0; i < 64; i++) {
        const uint8_t c = *(base + map_obj + i);
        txt[i] = (c >= 32 && c < 127) ? char(c) : '.';
      }
    }
    REXKRNL_WARN(
        "[shopguard] #{} DEAD MAP map={:#x} head={:#x} isnil={} key={} "
        "previously_live={} | \"{}\"",
        n, map_obj, head, isnil, int32_t(key), was_live ? 1 : 0, txt);
  }

  // Skip the descent (which would spin forever) and hand back a scratch value
  // block so the caller's iterator store stays well-formed.
  const uint32_t scratch = EnsureScratchValue();
  r3.u32 = scratch ? scratch + kNodeValue : map_obj + kNodeValue;
  return true;
}
