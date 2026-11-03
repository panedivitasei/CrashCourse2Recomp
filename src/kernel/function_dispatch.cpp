// Invalid-call tolerance, title-side.
//
// The runtime's ResolveIndirectFunction traps fatally on a call to an
// unregistered guest address. Offline, DCC2 makes such calls: the Kernel
// Dispatch thread enters guest code within ~0.1s of launch while an object is
// still unbuilt, and the store/economy paths call virtuals on objects that are
// never constructed without live content. Both are benign no-ops on hardware.
//
// The symbol is declared in rex/ppc/func.h with no dllimport, so the
// generated recomp code -- which is where indirect calls happen -- binds to
// this definition instead of the runtime's.

#include <atomic>
#include <cstdint>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

namespace {

// Return as if a void/0-returning function ran.
void NoOpCall(PPCContext& ctx, uint8_t* base) {
  const uint32_t target = ctx.last_indirect_target;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);

  // A virtual call's bad target came from *(*r3 + voffset), so r3 and its
  // vtable word name the unbuilt object. r3 is untrusted here: read the word
  // only if the page is committed and readable, or the diagnostic becomes its
  // own access violation.
  uint32_t vtable = 0;
  const uint32_t self = ctx.r3.u32;
  if (base && self >= 0x1000u && self < 0xC0000000u) {
    rex::Runtime* runtime = rex::Runtime::instance();
    rex::memory::BaseHeap* heap =
        runtime && runtime->memory() ? runtime->memory()->LookupHeap(self) : nullptr;
    uint32_t protect = 0;
    if (heap && heap->QueryProtect(self, &protect) &&
        (protect & rex::memory::kMemoryProtectRead)) {
      vtable = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + self));
    }
  }

  // Log each distinct caller once so the call sites are enumerable instead of
  // flooded by per-frame repeats. Racy fixed-size table, fine for a diagnostic.
  static uint32_t seen[128];
  static std::atomic<uint32_t> seen_n{0};
  const uint32_t n = seen_n.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < n && i < 128; ++i) {
    if (seen[i] == lr) return;
  }
  if (n < 128) {
    seen[n] = lr;
    seen_n.store(n + 1, std::memory_order_relaxed);
  }
  REXLOG_WARN("[cc2] invalid call to {:08X} from lr={:08X} (r3={:08X} vtable={:08X}), skipped",
              target, lr, self, vtable);
}

}  // namespace

namespace rex::runtime {

PPCFunc* ResolveIndirectFunction(uint32_t guest_address) {
  Runtime* runtime = Runtime::instance();
  FunctionDispatcher* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  if (dispatcher) {
    if (PPCFunc* func = dispatcher->GetFunction(guest_address)) {
      return func;
    }
  }
  return &NoOpCall;
}

}  // namespace rex::runtime
