// Title-exit diagnostics.
//
// The game exits to the dashboard through the XamLoader exports, which the
// runtime implements as a plain TerminateTitle with no record of why. Each
// hook logs the export, its arguments and the guest caller (lr), then chains
// to the runtime's original.

#include <cstdint>
#include <string>

#include <windows.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

namespace {

PPCFunc* OriginalExport(const char* name) {
  HMODULE runtime = GetModuleHandleW(L"rexruntimerd.dll");
  if (!runtime) runtime = GetModuleHandleW(L"rexruntime.dll");
  if (!runtime) return nullptr;
  return reinterpret_cast<PPCFunc*>(GetProcAddress(runtime, name));
}

std::string GuestString(uint32_t ptr) {
  if (ptr < 0x10000u || ptr >= 0xC0000000u) {
    return "<" + std::to_string(ptr) + ">";
  }
  const char* s = REX_KERNEL_MEMORY()->TranslateVirtual<const char*>(ptr);
  return s ? std::string(s, strnlen(s, 260)) : "<null>";
}

void Chain(PPCContext& ctx, uint8_t* base, const char* name) {
  static thread_local bool in_chain = false;
  REXKRNL_INFO("[loader] {} r3={:#x} r4={:#x} r5={:#x} r6={:#x} lr={:#x}", name, ctx.r3.u32,
               ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, static_cast<uint32_t>(ctx.lr));
  PPCFunc* original = OriginalExport(name);
  if (!original) {
    REXKRNL_WARN("[loader] {}: runtime has no export; returning", name);
    ctx.r3.u64 = 0;
    return;
  }
  original(ctx, base);
}

}  // namespace

REX_HOOK_RAW(__imp__XamLoaderTerminateTitle) {
  Chain(ctx, base, "__imp__XamLoaderTerminateTitle");
}

REX_HOOK_RAW(__imp__XamLoaderLaunchTitle) {
  REXKRNL_INFO("[loader] XamLoaderLaunchTitle path='{}'", GuestString(ctx.r3.u32));
  Chain(ctx, base, "__imp__XamLoaderLaunchTitle");
}

REX_HOOK_RAW(__imp__XamLoaderLaunchTitleEx) {
  REXKRNL_INFO("[loader] XamLoaderLaunchTitleEx path='{}'", GuestString(ctx.r3.u32));
  Chain(ctx, base, "__imp__XamLoaderLaunchTitleEx");
}
