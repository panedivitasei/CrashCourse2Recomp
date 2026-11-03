// Guest thread placement.
//
// A thread created without a processor mask lands on the parent's CPU on
// real hardware. The runtime round-robins those across its six fake cores
// instead, which scatters the game's worker threads and their timing. Set the
// mask to the parent's CPU before the runtime sees it.

#include <cstdint>

#include <windows.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/xthread.h>

namespace {

PPCFunc* OriginalExport(const char* name) {
  HMODULE runtime = GetModuleHandleW(L"rexruntimerd.dll");
  if (!runtime) runtime = GetModuleHandleW(L"rexruntime.dll");
  if (!runtime) return nullptr;
  return reinterpret_cast<PPCFunc*>(GetProcAddress(runtime, name));
}

}  // namespace

// ExCreateThread(handle, stack_size, thread_id, xapi_startup, start, context,
// creation_flags): the processor mask is the top byte of creation_flags (r9).
REX_HOOK_RAW(__imp__ExCreateThread) {
  static PPCFunc* original = OriginalExport("__imp__ExCreateThread");
  if (!original) {
    ctx.r3.u64 = 0x80000000u;  // X_STATUS_UNSUCCESSFUL
    return;
  }
  if ((ctx.r9.u32 >> 24) == 0) {
    if (auto* parent = rex::system::XThread::GetCurrentThread()) {
      const uint32_t cpu = parent->active_cpu() & 7u;
      ctx.r9.u32 |= (1u << cpu) << 24;
      REXKRNL_DEBUG("[affinity] new guest thread inherits cpu {}", cpu);
    }
  }
  original(ctx, base);
}
