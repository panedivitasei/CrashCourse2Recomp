// Tournaments run over a theme's first five levels, the courses the tally counts to; the theme's bonus levels follow
// them in theme order.

#include <cstdint>

#include <rex/hook.h>
#include <rex/types.h>

extern "C" REX_FUNC(__imp__sub_82B45D68);  // LEVELCONFIGMANAGER: reset the random tournament's remaining-level mask
extern "C" REX_FUNC(__imp__sub_82B46308);  // LEVELCONFIGMANAGER: select the next level; 0 when none is left

namespace {

constexpr uint32_t kTournamentMode = 1;  // manager +156
constexpr uint32_t kTournamentCourses = 5;

uint32_t Word(uint8_t* base, uint32_t address) { return *reinterpret_cast<rex::be<uint32_t>*>(base + address); }

}  // namespace

// Random order (manager +152 set): +160 holds one bit per theme position still to be played.
REX_HOOK_RAW(sub_82B45D68) {
  const uint32_t manager = ctx.r3.u32;
  __imp__sub_82B45D68(ctx, base);
  if (Word(base, manager + 156) == kTournamentMode && base[manager + 152]) {
    *reinterpret_cast<rex::be<uint32_t>*>(base + manager + 160) =
        Word(base, manager + 160) & ((1u << kTournamentCourses) - 1);
  }
}

// Theme order (manager +152 clear): the next level is position +52 plus one.
REX_HOOK_RAW(sub_82B46308) {
  const uint32_t manager = ctx.r3.u32;
  if (Word(base, manager + 156) == kTournamentMode && !base[manager + 152] &&
      int32_t(Word(base, manager + 52)) + 1 >= int32_t(kTournamentCourses)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82B46308(ctx, base);
}
