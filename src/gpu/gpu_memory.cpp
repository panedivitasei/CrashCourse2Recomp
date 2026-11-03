// Range tracker plus the resource-lifetime observer hooks. Each hook calls through first
// so the runtime's own header writes land before the range is read back out of the header.

#include "gpu/gpu_memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "gpu/gpu_texture_decode.h"
#include "gpu/gpu_tracker.h"
#include "gpu/guest/d3d_device.h"

extern "C" REX_FUNC(__imp__sub_82538150);
extern "C" REX_FUNC(__imp__sub_82538218);
extern "C" REX_FUNC(__imp__sub_82538228);
extern "C" REX_FUNC(__imp__sub_82538DD0);
extern "C" REX_FUNC(__imp__sub_82539330);
extern "C" REX_FUNC(__imp__sub_82539380);
extern "C" REX_FUNC(__imp__sub_8253A700);
extern "C" REX_FUNC(__imp__sub_8253A7D8);
extern "C" REX_FUNC(__imp__sub_8253B0E0);
extern "C" REX_FUNC(__imp__sub_8276E158);
extern "C" REX_FUNC(__imp__sub_8276E1C0);
extern "C" REX_FUNC(__imp__sub_8276E0E8);

namespace cc2::gpu::memory {

struct Range {
  const uint32_t end;
  std::atomic<uint32_t> generation;
  std::atomic<bool> active{true};

  Range(uint32_t end, uint32_t generation) : end(end), generation(generation) {}
};

namespace {

namespace xenos = rex::graphics::xenos;

std::mutex g_mutex;
std::map<uint32_t, std::shared_ptr<Range>> g_ranges;  // registered, keyed by base, non-overlapping
uint32_t g_generation = 1;
std::atomic<uint64_t> g_revision{1};

template <typename T>
const T* Guest(uint32_t address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<const T*>(address);
}

// Erases registered ranges overlapping [base, end) and returns whether any existed.
void EraseOverlaps(uint32_t base, uint32_t end) {
  auto it = g_ranges.upper_bound(base);
  if (it != g_ranges.begin()) {
    auto prev = std::prev(it);
    if (prev->second->end > base) it = prev;
  }
  while (it != g_ranges.end() && it->first < end) {
    it->second->active.store(false, std::memory_order_release);
    it = g_ranges.erase(it);
  }
}

// Physical data ranges of a resource header: up to two (texture base and mips).
int ResourceRanges(uint32_t resource_address, uint32_t (&base)[2], uint32_t (&size)[2]) {
  if (!resource_address) return 0;
  const auto* resource = Guest<guest::D3DResource>(resource_address);
  const uint32_t type = uint32_t(resource->Common) & 0xF;
  if (type == 1) {
    const auto* vb = Guest<guest::D3DVertexBuffer>(resource_address);
    base[0] = uint32_t(vb->Format[0]) & ~3u;
    size[0] = uint32_t(vb->Format[1]) & 0x3FFFFFC;
    // Engine-built headers over XPhysicalAlloc blocks hold the CPU view of the data; the draw reads the fetch
    // constant's physical address, so the range is keyed the way the draw looks it up.
    if (base[0] >= 0x20000000) {
      const uint32_t physical = REX_KERNEL_MEMORY()->GetPhysicalAddress(base[0]);
      base[0] = physical != UINT32_MAX ? physical : base[0] & 0x1FFFFFFF;
    }
    return base[0] && size[0] ? 1 : 0;
  }
  if (type == 2) {
    // Index buffers carry a virtual address (DrawIndexedVertices converts it for the GPU), so they are keyed by it.
    const auto* ib = Guest<guest::D3DIndexBuffer>(resource_address);
    base[0] = ib->Address;
    size[0] = ib->Size;
    return base[0] && size[0] ? 1 : 0;
  }
  if (type == 3) {
    const auto* texture = Guest<guest::D3DBaseTexture>(resource_address);
    xenos::xe_gpu_texture_fetch_t fetch;
    for (int i = 0; i < 6; ++i) (&fetch.dword_0)[i] = texture->Format.dword[i];
    uint32_t n = 0;
    uint32_t b, bs, m, ms;
    if (TextureMemory(fetch, b, bs, m, ms)) {
      if (b && bs) base[n] = b, size[n++] = bs;
      if (m && ms) base[n] = m, size[n++] = ms;
    }
    return int(n);
  }
  return 0;
}

}  // namespace

void Register(uint32_t base, uint32_t size) {
  if (!base || !size) return;
  std::lock_guard lock(g_mutex);
  // A header for a piece of a registered block keeps the block registered; only a range that does not fit inside
  // an existing one replaces what it overlaps.
  auto it = g_ranges.upper_bound(base);
  if (it != g_ranges.begin()) {
    auto prev = std::prev(it);
    if (prev->first <= base && prev->second->end >= base + size) {
      prev->second->generation.store(++g_generation, std::memory_order_release);
      return;
    }
  }
  EraseOverlaps(base, base + size);
  g_ranges[base] = std::make_shared<Range>(base + size, ++g_generation);
  g_revision.fetch_add(1, std::memory_order_release);
}

void MarkDirty(uint32_t base, uint32_t size) {
  if (!base || !size) return;
  std::lock_guard lock(g_mutex);
  const uint32_t end = base + size;
  auto it = g_ranges.upper_bound(base);
  if (it != g_ranges.begin()) {
    auto prev = std::prev(it);
    if (prev->second->end > base) it = prev;
  }
  for (; it != g_ranges.end() && it->first < end; ++it) {
    it->second->generation.store(++g_generation, std::memory_order_release);
  }
}

RangeState Query(uint32_t base, uint32_t size) {
  thread_local std::array<QueryCache, 512> cache;
  const uint64_t key = (uint64_t(base) << 32) | size;
  auto& cached = cache[XXH3_64bits(&key, sizeof(key)) & (cache.size() - 1)];
  return Query(base, size, cached);
}

RangeState Query(uint32_t base, uint32_t size, QueryCache& cached) {
  const uint64_t key = (uint64_t(base) << 32) | size;
  if (cached.key == key) {
    if (cached.range && cached.range->active.load(std::memory_order_acquire)) {
      return {true, cached.range->generation.load(std::memory_order_acquire)};
    }
    if (!cached.range && cached.revision == g_revision.load(std::memory_order_acquire)) return {};
  }

  // Cached owners publish dirty versions directly; replacement retires the old owner before erasing it.
  // Misses follow the map revision so a new registration cannot leave an unowned result cached.
  std::lock_guard lock(g_mutex);
  auto it = g_ranges.upper_bound(base);
  std::shared_ptr<Range> range;
  if (it != g_ranges.begin()) {
    --it;
    if (it->second->end >= base + size) range = it->second;
  }
  cached = {key, g_revision.load(std::memory_order_relaxed), std::move(range)};
  return cached.range ? RangeState{true, cached.range->generation.load(std::memory_order_relaxed)} : RangeState{};
}

void RegisterResource(uint32_t resource_address) {
  uint32_t base[2], size[2];
  const int n = ResourceRanges(resource_address, base, size);
  for (int i = 0; i < n; ++i) {
    tracker::InvalidateResolved(base[i], size[i]);
    Register(base[i], size[i]);
  }
}

void DirtyResource(uint32_t resource_address) {
  uint32_t base[2], size[2];
  const int n = ResourceRanges(resource_address, base, size);
  for (int i = 0; i < n; ++i) {
    tracker::InvalidateResolved(base[i], size[i]);
    MarkDirty(base[i], size[i]);
  }
}

}  // namespace cc2::gpu::memory

namespace memory = cc2::gpu::memory;

// D3DDevice_CreateVertexBuffer 0x82538150 (Length, Usage, Pool) returns the buffer.
REX_HOOK_RAW(sub_82538150) {
  __imp__sub_82538150(ctx, base);
  memory::RegisterResource(ctx.r3.u32);
}

// D3DVertexBuffer_Unlock 0x82538218 (pBuffer).
REX_HOOK_RAW(sub_82538218) {
  const uint32_t resource = ctx.r3.u32;
  __imp__sub_82538218(ctx, base);
  memory::DirtyResource(resource);
}

// D3D_CreateIndexBuffer 0x82538228 (Length, Usage, Format, Pool) returns the buffer.
REX_HOOK_RAW(sub_82538228) {
  __imp__sub_82538228(ctx, base);
  memory::RegisterResource(ctx.r3.u32);
}

// D3DResource_Release returns the remaining reference count; only the final release retires its memory.
REX_HOOK_RAW(sub_82538DD0) {
  uint32_t addresses[2], sizes[2];
  const int count = memory::ResourceRanges(ctx.r3.u32, addresses, sizes);
  __imp__sub_82538DD0(ctx, base);
  if (ctx.r3.u32 == 0) {
    for (int i = 0; i < count; ++i) {
      cc2::gpu::tracker::InvalidateResolved(addresses[i], sizes[i]);
      memory::MarkDirty(addresses[i], sizes[i]);
    }
  }
}

// D3DVertexBuffer_Lock 0x82539330 (pBuffer, Offset, Size, Flags).
REX_HOOK_RAW(sub_82539330) {
  const uint32_t resource = ctx.r3.u32;
  __imp__sub_82539330(ctx, base);
  memory::DirtyResource(resource);
}

// D3DIndexBuffer_Lock 0x82539380 (pBuffer, Offset, Size, Flags).
REX_HOOK_RAW(sub_82539380) {
  const uint32_t resource = ctx.r3.u32;
  __imp__sub_82539380(ctx, base);
  memory::DirtyResource(resource);
}

// D3D::LockSurface 0x8253A700 and Lock2DSurface 0x8253A7D8, first argument the texture.
REX_HOOK_RAW(sub_8253A700) {
  const uint32_t resource = ctx.r3.u32;
  __imp__sub_8253A700(ctx, base);
  memory::DirtyResource(resource);
}

REX_HOOK_RAW(sub_8253A7D8) {
  const uint32_t resource = ctx.r3.u32;
  __imp__sub_8253A7D8(ctx, base);
  memory::DirtyResource(resource);
}

// D3DDevice_CreateTexture 0x8253B0E0 (Width, Height, Depth, Levels, Usage, Format, Pool, Type) returns the texture.
REX_HOOK_RAW(sub_8253B0E0) {
  __imp__sub_8253B0E0(ctx, base);
  memory::RegisterResource(ctx.r3.u32);
}

// XGOffsetBaseTextureAddress 0x8276E158 (pTexture, pBase, pMip) and XGOffsetResourceAddress 0x8276E1C0 (pResource,
// pBase): the header now points at its data, so the range becomes owned.
REX_HOOK_RAW(sub_8276E158) {
  const uint32_t resource = ctx.r3.u32;
  __imp__sub_8276E158(ctx, base);
  memory::RegisterResource(resource);
}

REX_HOOK_RAW(sub_8276E1C0) {
  const uint32_t resource = ctx.r3.u32;
  __imp__sub_8276E1C0(ctx, base);
  memory::RegisterResource(resource);
}

// XGSetTextureHeader 0x8276E0E8 (Width, Height, Levels, Usage, Format, Pool, BaseOffset, MipOffset, Pitch, pTexture,
// pBaseSize, pMipSize); pTexture is the tenth argument, the stack slot at r1+0x58. The avatar header setup 0x829EB4B8
// builds its texture headers this way with absolute bases and no XGOffset call, and it runs again after every
// completed asset build, so the header's range is registered (and thereby dirtied) here.
REX_HOOK_RAW(sub_8276E0E8) {
  const uint32_t texture = *REX_KERNEL_MEMORY()->TranslateVirtual<rex::be<uint32_t>*>(ctx.r1.u32 + 0x58);
  __imp__sub_8276E0E8(ctx, base);
  if (texture) memory::RegisterResource(texture);
}
