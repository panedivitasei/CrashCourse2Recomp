// Commit-point capture: Clear, Resolve and Swap build immutable records from the D3D arguments and the device
// in effect, then call through to the runtime so its own bookkeeping stays intact.
// Argument registers follow the XDK d3d9.h prototypes; float arguments take a GPR slot, so late ones land on the stack.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "cc2/scene_driver.h"
#include "cc2/display_settings.h"
#include "cc2/thread/precise_sleep.h"
#include "gpu/gpu.h"
#include "gpu/gpu_cmdbuf.h"
#include "gpu/gpu_tracker.h"
#include "gpu/guest/d3d_device.h"

REXCVAR_DEFINE_INT32(gpu_fps_cap, 60, "GPU", "Pace Swap to this many frames per second; 0 leaves the game unpaced");

extern "C" REX_FUNC(__imp__sub_8253EAE0);
extern "C" REX_FUNC(__imp__sub_82540728);
extern "C" REX_FUNC(__imp__sub_82542220);
extern "C" REX_FUNC(__imp__sub_829FD2E0);
extern "C" REX_FUNC(__imp__sub_8253BDB0);
extern "C" REX_FUNC(__imp__sub_8253CDE8);
extern "C" REX_FUNC(__imp__sub_8253B6A0);
extern "C" REX_FUNC(__imp__sub_8253D208);

namespace {

using cc2::gpu::Rect;
using cc2::gpu::ResourceDesc;
using cc2::gpu::ResourceKind;
using cc2::gpu::guest::D3DDevice;
namespace tracker = cc2::gpu::tracker;

// D3DCLEAR_* and D3DRESOLVE_* from XDK d3d9types.h.
constexpr uint32_t kClearTarget0 = 0x1;
constexpr uint32_t kClearZBuffer = 0x10;
constexpr uint32_t kClearStencil = 0x20;
constexpr uint32_t kResolveSourceMask = 0x7;
constexpr uint32_t kResolveDepthStencil = 0x4;
constexpr uint32_t kResolveClearRenderTarget = 0x100;
constexpr uint32_t kResolveClearDepthStencil = 0x200;
constexpr int kResolveExponentBiasShift = 26;

template <typename T>
const T* Guest(uint32_t address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<const T*>(address);
}

// D3DRECT {x1, y1, x2, y2}, LONGs.
Rect ReadRect(uint32_t address) {
  const auto* v = Guest<rex::be<int32_t>>(address);
  return Rect{v[0], v[1], v[2], v[3]};
}

Rect ClampRect(Rect r, uint32_t width, uint32_t height) {
  r.left = std::clamp<int32_t>(r.left, 0, int32_t(width));
  r.right = std::clamp<int32_t>(r.right, r.left, int32_t(width));
  r.top = std::clamp<int32_t>(r.top, 0, int32_t(height));
  r.bottom = std::clamp<int32_t>(r.bottom, r.top, int32_t(height));
  return r;
}

void Once(std::atomic<bool>& flag, const char* what, uint32_t value) {
  if (!flag.exchange(true)) REXGPU_ERROR("[gpu] {} ({:#x})", what, value);
}

}  // namespace

namespace cc2::gpu::capture {

ClearRecord MakeClear(const D3DDevice* device, const ClearArgs& a) {
  ClearRecord record;
  for (uint32_t i = 0; i < 4; ++i) {
    if (a.flags & (kClearTarget0 << i)) {
      record.color[i] = tracker::Surface(device->render_targets[i], ResourceKind::kColorTarget);
    }
  }
  record.clear_depth = (a.flags & kClearZBuffer) != 0;
  record.clear_stencil = (a.flags & kClearStencil) != 0;
  if (record.clear_depth || record.clear_stencil) {
    record.depth = tracker::Surface(device->depth_stencil, ResourceKind::kDepthTarget);
  }
  record.color_value = {((a.color >> 16) & 0xFF) / 255.0f, ((a.color >> 8) & 0xFF) / 255.0f,
                        (a.color & 0xFF) / 255.0f, (a.color >> 24) / 255.0f};
  record.depth_value = a.z;
  record.stencil_value = a.stencil & 0xFF;
  record.rects = a.rects;
  return record;
}

}  // namespace cc2::gpu::capture

// D3DDevice_Clear 0x8253EAE0 (pDevice, Count, pRects, Flags, Color, Z, Stencil, EDRAMClear).
REX_HOOK_RAW(sub_8253EAE0) {
  cc2::gpu::draw::FinishPending();
  const uint32_t device = ctx.r3.u32;
  const uint32_t count = ctx.r4.u32;
  const uint32_t rects = ctx.r5.u32;
  cc2::gpu::capture::ClearArgs a;
  a.flags = ctx.r6.u32;
  a.color = ctx.r7.u32;
  a.z = float(ctx.f1.f64);
  a.stencil = ctx.r9.u32;
  if (rects) {
    for (uint32_t i = 0; i < count; ++i) a.rects.push_back(ReadRect(rects + i * 16));
  }
  cc2::gpu::cmdbuf::NotePending(device);
  if (cc2::gpu::cmdbuf::Recording(device)) {
    cc2::gpu::cmdbuf::RecordClear(device, a);
  } else {
    cc2::gpu::Submit(cc2::gpu::capture::MakeClear(Guest<D3DDevice>(device), a));
  }
  __imp__sub_8253EAE0(ctx, base);
}

// D3DDevice_BeginCommandBuffer 0x8253BDB0 (pDevice, pCommandBuffer, Flags, pInheritTags, pPersistTags, pTilingRects,
// TileCount) and EndCommandBuffer 0x8253CDE8 (pDevice): the recording window on that device.
REX_HOOK_RAW(sub_8253BDB0) {
  cc2::gpu::draw::FinishPending();
  cc2::gpu::cmdbuf::Begin(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  __imp__sub_8253BDB0(ctx, base);
}

REX_HOOK_RAW(sub_8253CDE8) {
  cc2::gpu::draw::FinishPending();
  const uint32_t device = ctx.r3.u32;
  __imp__sub_8253CDE8(ctx, base);
  cc2::gpu::cmdbuf::End(device);
}

// D3DDevice_SetCommandBufferPredication 0x8253B6A0 (pDevice, TilePredication, RunPredication).
REX_HOOK_RAW(sub_8253B6A0) {
  cc2::gpu::cmdbuf::SetPredication(ctx.r3.u32, ctx.r5.u32);
  __imp__sub_8253B6A0(ctx, base);
}

// D3DDevice_RunCommandBuffer 0x8253D208 (pDevice, pCommandBuffer, PredicationSelect).
REX_HOOK_RAW(sub_8253D208) {
  cc2::gpu::draw::FinishPending();
  cc2::gpu::cmdbuf::Run(ctx, base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  __imp__sub_8253D208(ctx, base);
}

// D3DDevice_Resolve 0x82540728 (pDevice, Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel, DestSliceOrFace,
// pClearColor, ClearZ, ClearStencil, pParameters); ClearStencil is the stack slot at r1+0x5C.
REX_HOOK_RAW(sub_82540728) {
  cc2::gpu::draw::FinishPending();
  static std::atomic<bool> s_depth_once{false};
  static std::atomic<bool> s_level_once{false};
  const auto* device = Guest<D3DDevice>(ctx.r3.u32);
  const uint32_t flags = ctx.r4.u32;
  const uint32_t source_index = flags & kResolveSourceMask;
  cc2::gpu::ResolveRecord record;
  record.source = source_index == kResolveDepthStencil
                      ? tracker::Surface(device->depth_stencil, ResourceKind::kDepthTarget)
                      : tracker::Surface(device->render_targets[source_index & 3], ResourceKind::kColorTarget);
  record.dest = tracker::Texture(ctx.r6.u32);
  const uint32_t level = ctx.r8.u32;
  const uint32_t slice = ctx.r9.u32;
  if (level || slice) Once(s_level_once, "resolve into a mip level or slice other than 0 is not handled yet and is skipped", level | (slice << 16));
  if (source_index == kResolveDepthStencil && record.dest.format != cc2::gpu::HostFormat::kR32Float) {
    Once(s_depth_once, "depth resolve into a non-depth texture is not handled yet; its destination shows magenta", flags);
  }
  const bool usable = record.source.id && record.dest.id && !level && !slice;
  if (usable) {
    const Rect full{0, 0, int32_t(record.source.width), int32_t(record.source.height)};
    record.source_rect = ClampRect(ctx.r5.u32 ? ReadRect(ctx.r5.u32) : full, record.source.width, record.source.height);
    if (ctx.r7.u32) {
      const auto* point = Guest<rex::be<int32_t>>(ctx.r7.u32);
      record.dest_x = point[0];
      record.dest_y = point[1];
    }
    const int bias = int32_t(flags) >> kResolveExponentBiasShift;
    record.scale = std::ldexp(1.0f, bias);
    record.clear_color = (flags & kResolveClearRenderTarget) && source_index != kResolveDepthStencil;
    if (record.clear_color && ctx.r10.u32) {
      const auto* v = Guest<rex::be<float>>(ctx.r10.u32);
      record.clear_color_value = {v[0], v[1], v[2], v[3]};
    }
    if (flags & kResolveClearDepthStencil) {
      record.clear_depth_target = tracker::Surface(device->depth_stencil, ResourceKind::kDepthTarget);
      record.clear_z = float(ctx.f1.f64);
      record.clear_stencil = *Guest<rex::be<uint32_t>>(ctx.r1.u32 + 0x5C) & 0xFF;
    }
    cc2::gpu::Submit(std::move(record));
  }
  __imp__sub_82540728(ctx, base);
}

// RENDERPASSDX9::SetupTilesForRendering 0x829FD2E0 (pass, target, colourCount, formats, width, height): a target that
// does not fit EDRAM gets band-high surfaces (colours +0x10..+0x1C, depth tile count +0x20 and array +0x24) and a tile
// count at +0x4C. The draws still arrive once with the full-target viewport and each per-tile resolve copies its own
// rect, so on the host every band surface is the whole target.
REX_HOOK_RAW(sub_829FD2E0) {
  const uint32_t target = ctx.r4.u32;
  const uint32_t colours = std::min<uint32_t>(ctx.r5.u32, 4);
  const uint32_t width = ctx.r7.u32, height = ctx.r8.u32;
  __imp__sub_829FD2E0(ctx, base);
  const auto* t = Guest<rex::be<uint32_t>>(target);
  const uint32_t tiles = t[0x4C / 4];
  if (!tiles) return;
  const uint32_t depth_count = std::min<uint32_t>(t[0x20 / 4], 16);
  const auto* depth_array = t[0x24 / 4] ? Guest<rex::be<uint32_t>>(t[0x24 / 4]) : nullptr;
  uint32_t depth[16] = {}, color[4] = {};
  for (uint32_t i = 0; depth_array && i < depth_count; ++i) depth[i] = depth_array[i];
  for (uint32_t i = 0; i < colours; ++i) color[i] = t[0x10 / 4 + i];
  tracker::RegisterTiledTarget(width, height, depth, depth_array ? depth_count : 0, color, colours);
}

// D3DDevice_Swap 0x82542220 (pDevice, pFrontBuffer, pParameters). Nothing throttles the game without a GPU, so an
// optional cap paces frames here; a frame that falls one behind restarts the deadline from now instead of catching up.
REX_HOOK_RAW(sub_82542220) {
  cc2::gpu::draw::FinishPending();
  using Clock = std::chrono::steady_clock;
  const int32_t cap = cc2::display::FrameLimit();
  if (cap > 0) {
    const auto kFrame = std::chrono::nanoseconds(1'000'000'000 / cap);
    static Clock::time_point deadline = Clock::now();
    deadline += kFrame;
    const auto now = Clock::now();
    if (deadline > now) {
      cc2::thread::SleepPrecise(deadline - now);
    } else if (now - deadline > kFrame) {
      deadline = now;
    }
  }
  // The scene tick runs before this frame is queued, so a dump it takes reads the previous presented frame.
  cc2::scene::OnSwap();
  cc2::gpu::SwapRecord record;
  record.front = tracker::Texture(ctx.r4.u32);
  record.frame = cc2::scene::FrameCount();
  cc2::gpu::WaitForWarmup();
  cc2::gpu::Submit(std::move(record));
  cc2::gpu::EndFrame();
  __imp__sub_82542220(ctx, base);
}
