// Hair under a hat. A hatted avatar's build carries the flattened hat hair as a hat part and the full hair as
// kHatlessHair; the draw list takes the full hair only while an instance shows hair but hides the hat (a jinx worn on
// the head). Instance masks default to every bit set, so the bit is decided here either way.

#include <cstdint>

#include <rex/hook.h>
#include <rex/types.h>

#include "kernel/xam/avatars/guest_asset.h"

extern "C" REX_FUNC(__imp__sub_829EEC90);  // AVATARMODEL: builds the draw list from the parts the mask at +704 shows

namespace {
namespace category = rex::avatars::ComponentCategory;
}  // namespace

REX_HOOK_RAW(sub_829EEC90) {
  auto& mask = *reinterpret_cast<rex::be<uint32_t>*>(base + ctx.r3.u32 + 704);
  const uint32_t shown = mask;
  const bool hatless = (shown & category::kHair) && !(shown & category::kHat);
  mask = hatless ? shown | category::kHatlessHair : shown & ~category::kHatlessHair;
  __imp__sub_829EEC90(ctx, base);
  mask = shown;
}
