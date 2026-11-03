// Sign-in notification.
//
// A console delivers XN_SYS_SIGNINCHANGED to a fresh listener because user 0
// is already signed in when the title starts listening; the game gates its
// boot on having seen it. The runtime only broadcasts it from XamShowSigninUI,
// which a title that already reports "signed in" never calls, so these hooks
// chain to the runtime's listener export and then deliver the sign-in state.

#include "cc2/online.h"
#include "cc2/kernel/xam/local_players.h"
#include <cstdint>

#include <windows.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

namespace {

constexpr uint32_t kXnSysSigninChanged = 0x0000000A;
constexpr uint32_t kXnLiveConnectionChanged = 0x02000001;
constexpr uint32_t kLogonConnectionEstablished = 0x001510F0;
constexpr uint64_t kLiveAreaMask = 0x00000002;

PPCFunc* OriginalExport(const char* name) {
  HMODULE runtime = GetModuleHandleW(L"rexruntimerd.dll");
  if (!runtime) runtime = GetModuleHandleW(L"rexruntime.dll");
  if (!runtime) return nullptr;
  return reinterpret_cast<PPCFunc*>(GetProcAddress(runtime, name));
}

// The kernel filters by area mask, so listeners outside SYS are untouched and
// re-delivery to earlier listeners is harmless.
void DeliverSigninState(uint64_t mask, uint32_t handle) {
  if (handle == 0) {
    return;
  }
  auto* kernel_state = rex::system::kernel_state();
  kernel_state->BroadcastNotification(kXnSysSigninChanged, cc2::kernel::xam::SignedInUserMask());
  // The LIVE link itself: the runtime never reports it, and live-gated flows
  // latch XN_LIVE_CONNECTIONCHANGED before they consider LIVE available.
  // Mask 0 subscribes to every area.
  if ((mask == 0 || (mask & kLiveAreaMask)) && cc2::online::ReportLive()) {
    kernel_state->BroadcastNotification(kXnLiveConnectionChanged, kLogonConnectionEstablished);
  }
  REXKRNL_DEBUG("Sign-in state delivered to listener {:08X} (mask {:#x})", handle, mask);
}

}  // namespace

REX_HOOK_RAW(__imp__XamNotifyCreateListener) {
  const uint64_t mask = ctx.r3.u64;
  static PPCFunc* original = OriginalExport("__imp__XamNotifyCreateListener");
  if (!original) {
    ctx.r3.u64 = 0;
    return;
  }
  original(ctx, base);
  DeliverSigninState(mask, ctx.r3.u32);
}

REX_HOOK_RAW(__imp__XamNotifyCreateListenerInternal) {
  const uint64_t mask = ctx.r3.u64;
  static PPCFunc* original = OriginalExport("__imp__XamNotifyCreateListenerInternal");
  if (!original) {
    ctx.r3.u64 = 0;
    return;
  }
  original(ctx, base);
  DeliverSigninState(mask, ctx.r3.u32);
}
