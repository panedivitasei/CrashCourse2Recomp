// Steps swinging ropes in equal parts: MCACCRope::Step's full 1/60 s slice followed by a short remainder drives its
// stiff springs unstable.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(cc2_rope_even_steps, true, "Gameplay", "Step ropes in equal parts of at most 1/120 s");

extern "C" REX_FUNC(__imp__sub_82AE83E8);  // MCACCRope::Step(this, dt)

REX_HOOK_RAW(sub_82AE83E8) {
  constexpr double kPart = 1.0 / 120.0;
  constexpr uint32_t kPush = 116;  // the player's swing push; Step clears it on exit
  const uint32_t mc = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  if (!REXCVAR_GET(cc2_rope_even_steps) || !(dt > kPart)) {
    __imp__sub_82AE83E8(ctx, base);
    return;
  }
  const int parts = std::min(int(std::ceil(dt / kPart - 1e-6)), 8);
  const uint32_t push = rex::memory::load_and_swap<uint32_t>(base + mc + kPush);
  for (int i = 0; i < parts; ++i) {
    rex::memory::store_and_swap<uint32_t>(base + mc + kPush, push);
    ctx.r3.u64 = mc;
    ctx.f1.f64 = dt / parts;
    __imp__sub_82AE83E8(ctx, base);
  }
}
