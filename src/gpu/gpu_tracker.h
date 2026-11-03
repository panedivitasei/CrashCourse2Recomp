// Guest-thread resource identity: which host resource stands for a guest surface or texture.
// Surfaces are keyed by their EDRAM placement (base, format, size, MSAA); textures by exact guest base address.
#pragma once

#include <cstdint>

#include "gpu/gpu_records.h"

namespace cc2::gpu::tracker {

// A render target or depth surface the game bound; kind is kColorTarget or kDepthTarget.
ResourceDesc Surface(uint32_t surface_address, ResourceKind kind);

// A target RENDERPASSDX9::SetupTilesForRendering split into bands: each of its EDRAM surfaces stands for the whole
// width x height target on the host, and every depth tile shares the first tile's host depth.
void RegisterTiledTarget(uint32_t width, uint32_t height, const uint32_t* depth_surfaces, uint32_t depth_count,
                         const uint32_t* color_surfaces, uint32_t color_count);

// The texture whose fetch constant sits in the D3DBaseTexture at texture_address; kNoResource if the header is unusable.
ResourceDesc Texture(uint32_t texture_address);

// A host texture that a resolve or swap already created at this physical base address, or kNoResource.
ResourceDesc FindResolved(uint32_t base_address, uint32_t width, uint32_t height, uint32_t guest_format);

// A CPU write or new resource owner invalidates host-only resolve contents overlapping this physical range.
void InvalidateResolved(uint32_t base, uint32_t size);

// Bumps whenever a resolve or swap texture is created or retired, so cached FindResolved answers know when to ask again.
uint32_t ResolveEpoch();

// A fresh host resource id, shared with the surface and texture ids above.
uint32_t NewId();

}  // namespace cc2::gpu::tracker
