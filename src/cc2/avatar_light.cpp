#include "cc2/avatar_light.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "dcc2_tu1_map.h"

REXCVAR_DEFINE_DOUBLE(cc2_hongkong_avatar_light, 0.4, "Display",
                      "Brightness of the player's avatar on Hong Kong courses (1 = the game's)");

extern "C" REX_FUNC(__imp__sub_82AC3CB8);  // RENDERER::SetupLights (base 0x82AB4938)
extern "C" REX_FUNC(__imp__sub_829EEE38);  // boned-model draw (base sub_829DFBF0)

namespace {

constexpr uint32_t kMaxForward = 16;

std::atomic<bool> g_hongkong{false};
thread_local bool t_avatar_draw = false;

uint32_t Read(uint32_t address) {
  if (address < 0x1000u || address >= 0xC0000000u) return 0;
  return *REX_KERNEL_MEMORY()->TranslateVirtual<const rex::be<uint32_t>*>(address);
}

void Write(uint32_t address, uint32_t value) {
  *REX_KERNEL_MEMORY()->TranslateVirtual<rex::be<uint32_t>*>(address) = value;
}

}  // namespace

namespace cc2::avatar_light {

void SceneLoaded(const std::string& path) {
  const bool hongkong = path.starts_with("dlc_hongkong/");
  if (g_hongkong.exchange(hongkong) != hongkong) {
    REXLOG_DEBUG("[avatar-light] {}", hongkong ? "Hong Kong course: avatar lighting scaled"
                                              : "left Hong Kong: avatar lighting as the game sets it");
  }
}

}  // namespace cc2::avatar_light

REX_HOOK_RAW(sub_829EEE38) {
  const bool outer = t_avatar_draw;
  t_avatar_draw = true;
  __imp__sub_829EEE38(ctx, base);
  t_avatar_draw = outer;
}

// For an avatar on a Hong Kong course, every forward light (light+472) and the packed ambient (r5) are scaled for
// this call only.
REX_HOOK_RAW(sub_82AC3CB8) {
  const float scale = float(REXCVAR_GET(cc2_hongkong_avatar_light));
  const uint32_t manager = t_avatar_draw && g_hongkong.load() && scale >= 0.0f && scale != 1.0f
                               ? Read(kGA_82E73C5C)
                               : 0;
  if (!manager) return __imp__sub_82AC3CB8(ctx, base);
  uint32_t lights[kMaxForward], saved[kMaxForward];
  uint32_t scaled = 0;
  const uint32_t count = Read(manager + 180);
  for (uint32_t i = 0; i < count && i < kMaxForward; ++i) {
    const uint32_t light = Read(manager + 48 + 4 * i);
    if (!light) continue;
    const uint32_t bits = Read(light + 472);
    float value;
    std::memcpy(&value, &bits, 4);
    value *= scale;
    uint32_t out;
    std::memcpy(&out, &value, 4);
    lights[scaled] = light;
    saved[scaled++] = bits;
    Write(light + 472, out);
  }
  // Packed ambient: alpha kept, colour bytes scaled.
  const uint32_t ambient = ctx.r5.u32;
  uint32_t dimmed = ambient & 0xFF000000u;
  for (int shift = 0; shift < 24; shift += 8) {
    const uint32_t channel = (ambient >> shift) & 0xFFu;
    dimmed |= uint32_t(std::min(255.0f, float(channel) * scale + 0.5f)) << shift;
  }
  ctx.r5.u64 = dimmed;
  __imp__sub_82AC3CB8(ctx, base);
  for (uint32_t i = 0; i < scaled; ++i) Write(lights[i] + 472, saved[i]);
  static std::atomic<bool> noted{false};
  if (scaled && !noted.exchange(true)) {
    REXLOG_DEBUG("[avatar-light] {} lights and ambient {:#010x} on an avatar at {:.2f}", scaled, ambient, scale);
  }
}
