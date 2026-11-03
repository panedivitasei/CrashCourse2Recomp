#include "gpu/gpu_tracker.h"
#include "gpu/gpu_resolve_cache.h"
#include "gpu/gpu_texture_decode.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>

#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "gpu/guest/d3d_device.h"

namespace cc2::gpu::tracker {

namespace {

namespace xenos = rex::graphics::xenos;

template <typename T>
const T* Guest(uint32_t address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<const T*>(address);
}

// EDRAM placement: surfaces the game creates at the same base, format and size share one host target, as they
// share the same EDRAM tiles on hardware. A different format at the same base stays separate; aliasing is not modelled.
struct SurfaceKey {
  ResourceKind kind;
  uint32_t edram_base;
  uint32_t format;      // colour or depth format field
  uint32_t dimensions;
  uint32_t msaa;
  auto operator<=>(const SurfaceKey&) const = default;
};

// A band surface of a tiled target: the host resource covers the whole target, keyed by the tile-0 surface.
struct TiledEntry {
  uint32_t width;
  uint32_t height;
  uint32_t canonical;   // surface whose EDRAM placement names the host resource
  uint32_t info;        // the band surface's ColorOrDepthInfo and Dimensions at registration: a freed and reused
  uint32_t dimensions;  // surface address no longer matches them, and the entry is dropped
};

std::mutex g_mutex;
std::map<SurfaceKey, ResourceDesc> g_surfaces;
std::unordered_map<uint32_t, TiledEntry> g_tiled;    // keyed by guest surface address
ResolveTextureCache g_textures;
uint32_t g_next_id = 1;

}  // namespace

ResourceDesc Surface(uint32_t surface_address, ResourceKind kind) {
  if (!surface_address) return {};
  const auto* surface = Guest<guest::D3DSurface>(surface_address);
  // A texture-level surface keeps its parent texture where the EDRAM fields would be; it cannot be bound as a target.
  if (uint32_t(surface->resource.Common) & guest::kSurfaceFromTexture) {
    static std::atomic<bool> once{false};
    if (!once.exchange(true)) REXGPU_ERROR("[gpu] surface {:#010x} bound as a target is a texture level", surface_address);
    return {};
  }
  std::lock_guard lock(g_mutex);
  // A band of a tiled target is the whole target on the host, placed where its tile-0 surface sits.
  const TiledEntry* tiled = nullptr;
  if (auto it = g_tiled.find(surface_address); it != g_tiled.end()) {
    const auto* canonical = Guest<guest::D3DSurface>(it->second.canonical);
    if (uint32_t(surface->ColorOrDepthInfo) == it->second.info && uint32_t(surface->Dimensions) == it->second.dimensions &&
        !(uint32_t(canonical->resource.Common) & guest::kSurfaceFromTexture)) {
      tiled = &it->second;
      surface = canonical;
    } else {
      g_tiled.erase(it);
    }
  }
  const uint32_t info = surface->ColorOrDepthInfo;
  const bool depth = kind == ResourceKind::kDepthTarget;
  // Base is the 12-bit tile index; the format sits at bit 16 (one bit for depth, four for colour).
  const SurfaceKey key{kind, info & 0xFFF, depth ? (info >> 16) & 1 : (info >> 16) & 0xF, uint32_t(surface->Dimensions),
                       uint32_t(surface->SurfaceInfo.get().msaa_samples)};
  if (auto it = g_surfaces.find(key); it != g_surfaces.end()) return it->second;
  ResourceDesc desc;
  desc.id = g_next_id++;
  desc.kind = kind;
  desc.width = tiled ? tiled->width : surface->width();
  desc.height = tiled ? tiled->height : surface->height();
  desc.samples = uint8_t(1u << std::min<uint32_t>(key.msaa, 2));
  desc.format = depth ? HostFormatForDepthTarget(key.format) : HostFormatForColorTarget(key.format);
  g_surfaces.emplace(key, desc);
  return desc;
}

void RegisterTiledTarget(uint32_t width, uint32_t height, const uint32_t* depth_surfaces, uint32_t depth_count,
                         const uint32_t* color_surfaces, uint32_t color_count) {
  std::lock_guard lock(g_mutex);
  auto add = [&](uint32_t address, uint32_t canonical) {
    if (!address || !canonical) return;
    const auto* s = Guest<guest::D3DSurface>(address);
    g_tiled[address] = TiledEntry{width, height, canonical, uint32_t(s->ColorOrDepthInfo), uint32_t(s->Dimensions)};
  };
  for (uint32_t i = 0; i < depth_count; ++i) add(depth_surfaces[i], depth_surfaces[0]);
  for (uint32_t i = 0; i < color_count; ++i) add(color_surfaces[i], color_surfaces[i]);
}

ResourceDesc Texture(uint32_t texture_address) {
  if (!texture_address) return {};
  const auto* texture = Guest<guest::D3DBaseTexture>(texture_address);
  xenos::xe_gpu_texture_fetch_t fetch;
  fetch.dword_0 = texture->Format.dword[0];
  fetch.dword_1 = texture->Format.dword[1];
  fetch.dword_2 = texture->Format.dword[2];
  fetch.dword_3 = texture->Format.dword[3];
  fetch.dword_4 = texture->Format.dword[4];
  fetch.dword_5 = texture->Format.dword[5];
  // Headers made by CreateTexture hold a CPU view of the data (0xE0000000 and up is offset by a page); SetTexture
  // converts it, so the draw side sees the physical address and the key here must match it.
  uint32_t base = fetch.base_address << 12;
  if (base >= 0x20000000) {
    const uint32_t physical = REX_KERNEL_MEMORY()->GetPhysicalAddress(base);
    base = physical != UINT32_MAX ? physical : base & 0x1FFFFFFF;
  }
  if (!base) return {};
  if (fetch.dimension != xenos::DataDimension::k2DOrStacked) {
    static std::atomic<bool> once{false};
    if (!once.exchange(true)) {
      REXGPU_ERROR("[gpu] texture {:#010x} has dimension {}; only its first 2D slice is tracked", texture_address,
                   uint32_t(fetch.dimension));
    }
  }
  ResourceDesc want;
  want.kind = ResourceKind::kTexture;
  want.width = fetch.size_2d.width + 1;
  want.height = fetch.size_2d.height + 1;
  const uint32_t guest_format = uint32_t(fetch.format);
  std::lock_guard lock(g_mutex);
  const ResourceDesc have = g_textures.Find(base, want.width, want.height, guest_format);
  if (have.id) return have;
  want.format = HostFormatForTexture(guest_format);
  want.id = g_next_id++;
  uint32_t physical, size, mip, mip_size;
  TextureMemory(fetch, physical, size, mip, mip_size);
  g_textures.Store(base, std::max(size, 1u), want, guest_format);
  return want;
}

uint32_t ResolveEpoch() { return g_textures.Epoch(); }

ResourceDesc FindResolved(uint32_t base_address, uint32_t width, uint32_t height, uint32_t guest_format) {
  std::lock_guard lock(g_mutex);
  return g_textures.Find(base_address, width, height, guest_format);
}

void InvalidateResolved(uint32_t base, uint32_t size) {
  if (!base || !size) return;
  std::lock_guard lock(g_mutex);
  g_textures.Invalidate(base, size);
}

uint32_t NewId() {
  std::lock_guard lock(g_mutex);
  return g_next_id++;
}

}  // namespace cc2::gpu::tracker
