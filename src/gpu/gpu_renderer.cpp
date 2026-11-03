// Host side of the native renderer: the plume device on the game window, the frame queue and the render thread
// that turns records into command lists. Nothing here reads guest memory.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <deque>
#include <fstream>
#include <unordered_set>
#include <sstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>

#include <plume_render_interface.h>

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

#include "gpu/gpu.h"
#include "cc2/display_settings.h"
#include "gpu/gpu_draw.h"
#include "gpu/gpu_overlay_pass.h"
#include "gpu/gpu_pointer.h"
#include "gpu/gpu_shader_pack.h"
#include "gpu/gpu_texture_decode.h"
#include "gpu/shaders/blit_shaders.h"
#include "gpu/shaders/resolve_shaders.h"
#include "gpu/shaders/resize_shaders.h"

REXCVAR_DEFINE_INT32(gpu_resolution_scale, 100, "GPU",
                     "Internal render-target resolution in percent of the game's, 20-200 (above 100 supersamples); changes apply at "
                     "a frame boundary");
REXCVAR_DEFINE_INT32(gpu_shadow_resolution_scale, 100, "GPU",
                     "Course shadow map resolution in percent of the game's 1024x1024, 50-400; changes apply at a "
                     "frame boundary");
REXCVAR_DEFINE_INT32(gpu_msaa, 0, "GPU",
                     "Host MSAA samples for the game's render targets: 0 = the game's own, 2, 4 or 8; read at device "
                     "creation");
REXCVAR_DEFINE_STRING(gpu_adapter, "", "GPU", "Adapter name substring; empty picks plume's default adapter");
REXCVAR_DEFINE_BOOL(gpu_fps, false, "GPU", "Draw the presented frame rate over the frame, in the game's own font");
REXCVAR_DEFINE_DOUBLE(gpu_pointer_size, 10.0, "GPU", "Mouse pointer height as a percentage of the frame height");
REXCVAR_DEFINE_STRING(gpu_pointer, "model", "GPU",
                      "Mouse pointer image: selector (the menu's own hand sprite), model (a glove baked from the "
                      "pointer0 mesh) or off");
REXCVAR_DEFINE_DOUBLE(gpu_pointer_angle, 45.0, "GPU",
                      "Degrees the hand is turned clockwise from pointing left; 45 points up-left, -80 is the game's "
                      "own tutorial hand");
REXCVAR_DEFINE_BOOL(gpu_constant_reuse, true, "GPU", "Reuse identical float constant uploads within a packet");
REXCVAR_DEFINE_BOOL(gpu_pipeline_warmup, true, "GPU",
                    "Record every draw pipeline beside the shader pack and compile the recorded set at start-up");
REXCVAR_DEFINE_INT32(gpu_pipeline_warmup_wait, 120, "GPU",
                     "Seconds the first frame may wait for the recorded pipelines to finish compiling; 0 = no wait");
REXCVAR_DEFINE_BOOL(gpu_async_pipelines, true, "GPU",
                    "Compile frame pipelines ahead on workers and wait before drawing with an unfinished pipeline");
REXCVAR_DEFINE_BOOL(gpu_vsync, true, "GPU", "Wait for vblank on present; the Swap hook paces to 60 Hz either way");

namespace plume {
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
}  // namespace plume

namespace cc2::gpu {

namespace {

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;

constexpr uint32_t kFramesInFlight = 2;
constexpr size_t kMaxQueuedFrames = 2;
constexpr plume::RenderFormat kSwapChainFormat = plume::RenderFormat::B8G8R8A8_UNORM;

// Shared memory: 128 MB, the clamp compiled into every shader. The low part holds the guest's registered vertex
// ranges; the two frame slots fill the transient area above it from opposite ends.
constexpr uint64_t kSharedMemorySize = 0x8000000;
constexpr uint64_t kPersistentSize = 0x4000000;
constexpr uint64_t kTransientSize = kSharedMemorySize - kPersistentSize;
static_assert(kFramesInFlight == 2, "the transient area is a two-ended stack");
constexpr uint64_t kUploadChunkSize = 32ull << 20;

// Descriptor heap: every slot is prefilled; the directory pack clamps texture indices at 2047, the container at
// 16383, and each clamp target holds a 2D blank. Cube and 3D blanks sit right below 2047.
constexpr uint32_t kTextureHeap = 16384;
constexpr uint32_t kSamplerHeap = 1024;  // one append-only table of exact sampler states
constexpr uint32_t kBlank2D = 0;
constexpr uint32_t kBlankCube = 2046;
constexpr uint32_t kBlank3D = 2045;

// Root descriptor order in the D3D12 draw layout.
enum RootSlot : uint32_t {
  kRootSystem,
  kRootFloatsVs,
  kRootFloatsPs,
  kRootBoolLoop,
  kRootFetch,
  kRootIndicesVs,
  kRootIndicesPs,
  kRootShared,
  kRootSharedUav,
  kRootVfetch,
  kRootCount,
};

// Mirrors the Constants block in shaders/blit.hlsl.
struct BlitConstants {
  float uv_offset[2];
  float uv_scale[2];
  float scale[4];
  uint32_t linear_filter;
};

// Mirrors the Constants block in shaders/resolve.hlsl; it shares the blit's push-constant range.
struct ResolveConstants {
  int32_t source_offset[2];
  uint32_t samples;
  uint32_t average;
  float scale[4];
};
static_assert(sizeof(ResolveConstants) <= sizeof(BlitConstants));

// Sampler sets: every draw binds a set holding exactly its samplers, cached by content. The D3D12 sampler heap holds
// 1024 descriptors, 16 per set.

plume::RenderFormat ToPlume(HostFormat format) {
  switch (format) {
    case HostFormat::kUnsupported: return plume::RenderFormat::R8G8B8A8_UNORM;
    case HostFormat::kRGBA8: return plume::RenderFormat::R8G8B8A8_UNORM;
    case HostFormat::kRGB10A2: return plume::RenderFormat::R10G10B10A2_UNORM;
    case HostFormat::kRGBA16Float: return plume::RenderFormat::R16G16B16A16_FLOAT;
    case HostFormat::kRG16Float: return plume::RenderFormat::R16G16_FLOAT;
    case HostFormat::kR32Float: return plume::RenderFormat::R32_FLOAT;
    case HostFormat::kRG32Float: return plume::RenderFormat::R32G32_FLOAT;
    case HostFormat::kR8: return plume::RenderFormat::R8_UNORM;
    case HostFormat::kRG8: return plume::RenderFormat::R8G8_UNORM;
    case HostFormat::kDepth32FloatStencil8: return plume::RenderFormat::D32_FLOAT_S8_UINT;
    case HostFormat::kBC1: return plume::RenderFormat::BC1_UNORM;
    case HostFormat::kBC2: return plume::RenderFormat::BC2_UNORM;
    case HostFormat::kBC3: return plume::RenderFormat::BC3_UNORM;
    case HostFormat::kBC4: return plume::RenderFormat::BC4_UNORM;
    case HostFormat::kBC5: return plume::RenderFormat::BC5_UNORM;
    case HostFormat::kRGBA16: return plume::RenderFormat::R16G16B16A16_UNORM;
    case HostFormat::kRG16: return plume::RenderFormat::R16G16_UNORM;
    case HostFormat::kR16Float: return plume::RenderFormat::R16_FLOAT;
    case HostFormat::kRGBA32Float: return plume::RenderFormat::R32G32B32A32_FLOAT;
    case HostFormat::kR16: return plume::RenderFormat::R16_UNORM;
  }
  return plume::RenderFormat::R8G8B8A8_UNORM;
}

plume::RenderComparisonFunction ToPlumeCompare(uint32_t f) {
  static constexpr plume::RenderComparisonFunction kMap[8] = {
      plume::RenderComparisonFunction::NEVER,     plume::RenderComparisonFunction::LESS,
      plume::RenderComparisonFunction::EQUAL,     plume::RenderComparisonFunction::LESS_EQUAL,
      plume::RenderComparisonFunction::GREATER,   plume::RenderComparisonFunction::NOT_EQUAL,
      plume::RenderComparisonFunction::GREATER_EQUAL, plume::RenderComparisonFunction::ALWAYS};
  return kMap[f & 7];
}

plume::RenderStencilOp ToPlumeStencil(uint32_t op) {
  static constexpr plume::RenderStencilOp kMap[8] = {
      plume::RenderStencilOp::KEEP,           plume::RenderStencilOp::ZERO,
      plume::RenderStencilOp::REPLACE,        plume::RenderStencilOp::INCREMENT_AND_CLAMP,
      plume::RenderStencilOp::DECREMENT_AND_CLAMP, plume::RenderStencilOp::INVERT,
      plume::RenderStencilOp::INCREMENT_AND_WRAP, plume::RenderStencilOp::DECREMENT_AND_WRAP};
  return kMap[op & 7];
}

plume::RenderBlend ToPlumeBlend(xenos::BlendFactor f) {
  using B = plume::RenderBlend;
  switch (f) {
    case xenos::BlendFactor::kZero: return B::ZERO;
    case xenos::BlendFactor::kOne: return B::ONE;
    case xenos::BlendFactor::kSrcColor: return B::SRC_COLOR;
    case xenos::BlendFactor::kOneMinusSrcColor: return B::INV_SRC_COLOR;
    case xenos::BlendFactor::kSrcAlpha: return B::SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcAlpha: return B::INV_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor: return B::DEST_COLOR;
    case xenos::BlendFactor::kOneMinusDstColor: return B::INV_DEST_COLOR;
    case xenos::BlendFactor::kDstAlpha: return B::DEST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstAlpha: return B::INV_DEST_ALPHA;
    case xenos::BlendFactor::kConstantColor:
    case xenos::BlendFactor::kConstantAlpha: return B::BLEND_FACTOR;
    case xenos::BlendFactor::kOneMinusConstantColor:
    case xenos::BlendFactor::kOneMinusConstantAlpha: return B::INV_BLEND_FACTOR;
    case xenos::BlendFactor::kSrcAlphaSaturate: return B::SRC_ALPHA_SAT;
  }
  return B::ONE;
}

// Alpha factors cannot name a colour on the host; the colour factors fall back to their alpha counterparts.
plume::RenderBlend ToPlumeAlphaBlend(xenos::BlendFactor f) {
  using B = plume::RenderBlend;
  switch (f) {
    case xenos::BlendFactor::kSrcColor: return B::SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcColor: return B::INV_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor: return B::DEST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstColor: return B::INV_DEST_ALPHA;
    default: return ToPlumeBlend(f);
  }
}

plume::RenderBlendOperation ToPlumeBlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kAdd: return plume::RenderBlendOperation::ADD;
    case xenos::BlendOp::kSubtract: return plume::RenderBlendOperation::SUBTRACT;
    case xenos::BlendOp::kMin: return plume::RenderBlendOperation::MIN;
    case xenos::BlendOp::kMax: return plume::RenderBlendOperation::MAX;
    case xenos::BlendOp::kRevSubtract: return plume::RenderBlendOperation::REV_SUBTRACT;
  }
  return plume::RenderBlendOperation::ADD;
}

plume::RenderSwizzle ToPlumeSwizzle(uint8_t s) {
  switch (s) {
    case 0: return plume::RenderSwizzle::R;
    case 1: return plume::RenderSwizzle::G;
    case 2: return plume::RenderSwizzle::B;
    case 3: return plume::RenderSwizzle::A;
    case 4: return plume::RenderSwizzle::ZERO;
    default: return plume::RenderSwizzle::ONE;
  }
}

struct HostResource {
  ResourceDesc desc;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  uint32_t width = 0;                                      // host texels: guest size times the resource's scale
  uint32_t height = 0;
  uint32_t scale = 100;                                    // percent: the resolution or shadow scale it was made at
  uint32_t samples = 1;
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  std::unique_ptr<plume::RenderDescriptorSet> sample_set;  // made on first blit
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  uint32_t version = 0;                                    // sampled textures: contents last uploaded
  std::unordered_map<uint32_t, uint32_t> views;            // (dimension, swizzle) -> descriptor index
  std::vector<std::unique_ptr<plume::RenderTextureView>> view_objects;
  bool depth() const { return desc.kind == ResourceKind::kDepthTarget; }
  bool sampled_only() const { return desc.kind == ResourceKind::kSampledTexture; }
  bool cube() const { return desc.shape == TextureShape::kCube; }
  bool volume() const { return desc.shape == TextureShape::k3D; }
  uint32_t layers() const { return cube() ? 6 : desc.shape == TextureShape::kStacked ? std::max(desc.depth, 1u) : 1; }
  uint32_t mips() const { return std::max(desc.mip_levels, 1u); }
};

// SamplerBinding::state (gpu_records.h) to a host sampler. Mirrored clamps without a host twin use MIRROR_ONCE, and
// the halfway clamps clamp to the edge.
plume::RenderSamplerDesc SamplerDescFor(uint32_t state) {
  using A = plume::RenderTextureAddressMode;
  auto address = [](uint32_t clamp) {
    switch (xenos::ClampMode(clamp)) {
      case xenos::ClampMode::kRepeat: return A::WRAP;
      case xenos::ClampMode::kMirroredRepeat: return A::MIRROR;
      case xenos::ClampMode::kMirrorClampToEdge:
      case xenos::ClampMode::kMirrorClampToHalfway:
      case xenos::ClampMode::kMirrorClampToBorder: return A::MIRROR_ONCE;
      case xenos::ClampMode::kClampToBorder: return A::BORDER;
      default: return A::CLAMP;
    }
  };
  plume::RenderSamplerDesc s;
  s.addressU = address(state & 7);
  s.addressV = address((state >> 3) & 7);
  s.addressW = address((state >> 6) & 7);
  s.magFilter = (state >> 9) & 1 ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
  s.minFilter = (state >> 10) & 1 ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
  const uint32_t mip = (state >> 11) & 3;
  s.mipmapMode = mip == 1 ? plume::RenderMipmapMode::LINEAR : plume::RenderMipmapMode::NEAREST;
  const uint32_t aniso = (state >> 13) & 7;
  s.anisotropyEnabled = aniso != 0;
  s.maxAnisotropy = aniso ? 1u << (aniso - 1) : 1;
  // Xenos border colours: ABGR black and white; the YCbCr blacks have no host equivalent and stay black.
  s.borderColor = ((state >> 16) & 3) == uint32_t(xenos::BorderColor::k_ABGR_White)
                      ? plume::RenderBorderColor::OPAQUE_WHITE
                      : plume::RenderBorderColor::TRANSPARENT_BLACK;
  s.minLOD = float((state >> 18) & 15);
  s.maxLOD = mip == 2 ? s.minLOD : float((state >> 22) & 15);
  if (s.maxLOD < s.minLOD) s.maxLOD = s.minLOD;
  return s;
}

// Letterbox a source aspect into the target, centred.
void FitRect(uint32_t target_w, uint32_t target_h, uint32_t source_w, uint32_t source_h, plume::RenderRect& out) {
  uint32_t w = target_w;
  uint32_t h = target_h;
  if (source_w && source_h) {
    if (uint64_t(target_w) * source_h > uint64_t(target_h) * source_w) {
      w = uint32_t(uint64_t(target_h) * source_w / source_h);
    } else {
      h = uint32_t(uint64_t(target_w) * source_h / source_w);
    }
  }
  const int32_t x = int32_t(target_w - w) / 2;
  const int32_t y = int32_t(target_h - h) / 2;
  out = plume::RenderRect(x, y, x + int32_t(w), y + int32_t(h));
}

// A CPU-visible buffer carved per frame slot; chunks are added on overflow and reset once the slot's fence passes.
struct UploadChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  uint8_t* mapped = nullptr;
  uint64_t size = 0;
  uint64_t used = 0;
};

struct UploadSpan {
  plume::RenderBuffer* buffer = nullptr;
  uint64_t offset = 0;
  uint8_t* data = nullptr;
};

// First-fit allocator over the persistent part of shared memory; frees wait until the frames using them retire.
class RangeAllocator {
 public:
  void Reset(uint64_t size) {
    free_.clear();
    free_[0] = size;
    pending_.clear();
  }
  bool Allocate(uint64_t size, uint64_t& offset) {
    size = (size + 255) & ~255ull;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
      if (it->second < size) continue;
      offset = it->first;
      const uint64_t rest = it->second - size;
      free_.erase(it);
      if (rest) free_[offset + size] = rest;
      return true;
    }
    return false;
  }
  void Free(uint64_t offset, uint64_t size, uint64_t frame) { pending_.push_back({offset, (size + 255) & ~255ull, frame}); }
  void Retire(uint64_t completed_frame) {
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->frame > completed_frame) {
        ++it;
        continue;
      }
      Insert(it->offset, it->size);
      it = pending_.erase(it);
    }
  }

 private:
  void Insert(uint64_t offset, uint64_t size) {
    auto next = free_.lower_bound(offset);
    if (next != free_.end() && offset + size == next->first) {
      size += next->second;
      next = free_.erase(next);
    }
    if (next != free_.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second == offset) {
        prev->second += size;
        return;
      }
    }
    free_[offset] = size;
  }
  struct Pending {
    uint64_t offset, size, frame;
  };
  std::map<uint64_t, uint64_t> free_;
  std::vector<Pending> pending_;
};

struct PersistentRange {
  uint32_t version = 0;
  uint64_t offset = 0;
  uint64_t size = 0;
  uint64_t last_frame = 0;  // frame of the last draw that referenced it, for eviction
};

std::mutex g_evicted_mutex;
std::vector<uint64_t> g_evicted;  // keys dropped by EvictPersistent or found missing at Place
std::atomic<bool> g_evicted_any{false};

void NoteEvicted(uint64_t key) {
  std::lock_guard lock(g_evicted_mutex);
  g_evicted.push_back(key);
  g_evicted_any.store(true, std::memory_order_release);
}
std::atomic<int> g_vsync_request{-1};  // -1 none, else the requested state, taken by the host before a present
std::atomic<float> g_pointer_x{0.0f}, g_pointer_y{0.0f};
std::atomic<bool> g_pointer_visible{false};
std::atomic<bool> g_pointer_suppressed{false};
std::atomic<int64_t> g_pointer_idle_start_ns{0};
std::atomic<uint64_t> g_frame_index{0};

class Backend {
 public:
  bool Create(rex::ui::Window* window);
  uint32_t PendingWarmups() const { return warm_pending_.load(std::memory_order_acquire); }
  void Destroy();
  void Execute(Packet& packet);
  void RequestResize() { resize_requested_.store(true, std::memory_order_release); }

 private:
  void Execute(Record& record);
  void Execute(const ClearRecord& r);
  void Execute(const ResolveRecord& r);
  void Execute(const SwapRecord& r);
  void Execute(DrawRecord& r);

  HostResource* Get(const ResourceDesc& desc);
  plume::RenderPipeline* BlitPipeline(plume::RenderFormat format, bool blend = false);
  plume::RenderDescriptorSet* SampleSet(HostResource& resource);
  void Blit(HostResource& source, const plume::RenderRect& source_rect, plume::RenderTexture* target,
            plume::RenderFramebuffer* framebuffer, plume::RenderFormat target_format, const plume::RenderRect& dest_rect,
            float scale, bool linear, bool blend = false, HostResource* owner = nullptr);
  void ResolveSamples(HostResource& source, int32_t sx, int32_t sy, HostResource& dest,
                      const plume::RenderRect& dest_rect, float scale);
  plume::RenderPipeline* ResolvePipeline(plume::RenderFormat format);

  // Resolution scale, course shadow scale and MSAA (gpu_resolution_scale, gpu_shadow_resolution_scale, gpu_msaa); all
  // are identity at their defaults.
  static int32_t Scaled(uint32_t scale, int32_t v) {
    return scale == 100 ? v : int32_t((int64_t(v) * scale + 50) / 100);
  }
  static int32_t Scaled(const HostResource& resource, int32_t v) { return Scaled(resource.scale, v); }
  // The sun's 1024x1024 shadow map (ShadowPass): every surface of its pass and the texture it resolves into, so the
  // pass's attachments stay one size.
  static bool CourseShadow(const ResourceDesc& desc) {
    return desc.width == 1024 && desc.height == 1024 && desc.shape == TextureShape::k2D &&
           desc.kind != ResourceKind::kSampledTexture;
  }
  uint32_t ScaleFor(const ResourceDesc& desc) const { return CourseShadow(desc) ? shadow_scale_pct_ : scale_pct_; }
  uint32_t HostSamples(const ResourceDesc& desc) const;
  void ResizeTargets(uint32_t scale, uint32_t shadow_scale);
  plume::RenderPipeline* ResizePipeline(const HostResource& source, uint32_t stencil_mask);
  std::vector<plume::RenderRect> ClipRects(const std::vector<Rect>& rects, const HostResource& target) const;
  plume::RenderDescriptorSet* SamplerSet(const DrawRecord& r, std::array<uint32_t, 32> (&indices)[2]);
  void UpdateFps();
  void DrawFps(plume::RenderTexture* back, plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height);
  void UpdatePointer();
  void DrawPointer(plume::RenderTexture* target, plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height,
                   HostResource* owner);

  // The native mouse pointer: the glove sprite baked from the game's pointer0 assets, blitted at the guest's position.
  struct PointerOverlay {
    bool tried = false;
    bool ok = false;
    PointerSprite sprite;
    HostResource texture;
  } pointer_;
  void BindTarget(HostResource& resource);
  void Unbind();
  void Transition(HostResource& resource, plume::RenderTextureLayout layout);
  void FlushTransitions();

  // Draw machinery.
  bool CreateDrawResources();
  void PrepareUploads(Packet& packet);
  bool Place(StreamData& stream, uint64_t& offset, std::vector<std::pair<uint64_t, UploadSpan>>& copies,
             std::vector<uint64_t>& copy_sizes);
  UploadSpan Upload(uint64_t size, uint64_t align);
  void ResetUploads(uint32_t slot);
  OverlayAllocator OverlayUploads() {
    return [this](uint64_t size, uint64_t align) {
      const UploadSpan span = Upload(size, align);
      return OverlayUpload{span.buffer, span.offset, span.data};
    };
  }
  bool AllocateTransient(uint64_t size, uint64_t& offset);
  uint32_t DescriptorFor(HostResource& resource, uint8_t dimension, const std::array<uint8_t, 4>& swizzle);
  uint32_t AllocateDescriptor();
  plume::RenderShader* Shader(const ShaderEntry* entry, bool trimmed);
  plume::RenderPipeline* DrawPipeline(const DrawRecord& r, uint32_t samples, bool wait = true);
  void RecordPipeline(const DrawRecord& r, uint32_t samples);
  void WarmPipelines();
  std::unordered_set<uint64_t> recorded_pipelines_;
  bool warming_ = false;
  plume::RenderFramebuffer* DrawFramebuffer(const DrawRecord& r);

  void BeginList();
  void SubmitAndWait();
  void WaitAll();
  bool BuildSwapChainTargets();
  void RebuildSwapChain();

  std::unique_ptr<plume::RenderInterface> interface_;
  std::unique_ptr<plume::RenderDevice> device_;
  std::unique_ptr<plume::RenderCommandQueue> queue_;
  std::unique_ptr<plume::RenderSwapChain> swap_chain_;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> swap_framebuffers_;
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> render_semaphores_;
  std::unique_ptr<plume::RenderCommandList> lists_[kFramesInFlight];
  std::unique_ptr<plume::RenderCommandFence> fences_[kFramesInFlight];
  std::unique_ptr<plume::RenderCommandSemaphore> acquire_semaphores_[kFramesInFlight];
  bool submitted_[kFramesInFlight] = {};
  struct ConstantUpload {
    const void* source;
    UploadSpan span;
  };
  std::unordered_multimap<uint64_t, ConstantUpload> constant_uploads_;
  uint32_t slot_ = 0;
  bool list_open_ = false;
  plume::RenderFramebuffer* bound_ = nullptr;
  std::vector<plume::RenderTextureBarrier> pending_barriers_;

  std::unique_ptr<plume::RenderSampler> point_sampler_;
  std::unique_ptr<plume::RenderSampler> linear_sampler_;
  plume::RenderDescriptorSetBuilder sample_set_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  OverlayPass overlay_;
  std::unique_ptr<plume::RenderShader> vs_;
  std::unique_ptr<plume::RenderShader> ps_;
  std::unique_ptr<plume::RenderShader> resolve_vs_;
  std::unique_ptr<plume::RenderShader> resolve_ps_;
  std::array<std::unique_ptr<plume::RenderShader>, 6> resize_shaders_;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> resize_pipelines_;
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> pipelines_;
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> resolve_pipelines_;
  uint32_t scale_pct_ = 100;
  uint32_t shadow_scale_pct_ = 100;
  uint64_t failed_scales_ = 0;  // the scale pair ResizeTargets gave up on, so a failure is not retried with a stall per frame
  uint32_t msaa_ = 0;  // host samples forced on render targets, 0 = the game's own

  // Sampler sets by content; a set unused for a full ring of frames can be rewritten.
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderSampler>> sampler_objects_;
  std::unordered_map<uint32_t, uint32_t> sampler_slots_;  // SamplerBinding::state -> slot in sampler_set_
  uint32_t sampler_slot_count_ = 0;
  uint32_t sampler_capacity_ = 16;                 // slots the pack's shaders can address
  std::vector<uint32_t> sampler_slot_state_;       // slot -> state it holds
  std::vector<uint64_t> sampler_slot_frame_;       // slot -> frame_counter_ of its last use

  std::unordered_map<uint32_t, HostResource> resources_;

  // Draws.
  bool draws_ready_ = false;
  std::unique_ptr<plume::RenderPipelineLayout> draw_layout_;
  plume::RenderDescriptorSetBuilder sampler_set_builder_;
  plume::RenderDescriptorSetBuilder texture_set_builder_;
  std::unique_ptr<plume::RenderDescriptorSet> sampler_set_;
  std::unique_ptr<plume::RenderDescriptorSet> texture_set_;
  std::vector<std::unique_ptr<plume::RenderSampler>> samplers_;
  std::unique_ptr<plume::RenderTexture> blank_2d_, blank_cube_, blank_3d_;
  std::unique_ptr<plume::RenderTextureView> blank_2d_view_, blank_cube_view_, blank_3d_view_;
  std::unique_ptr<plume::RenderBuffer> shared_memory_;
  std::unique_ptr<plume::RenderBuffer> dummy_uav_;
  std::vector<UploadChunk> uploads_[kFramesInFlight];
  RangeAllocator persistent_;
  bool EvictPersistent(uint64_t size, uint64_t& offset);
  std::unordered_map<uint64_t, PersistentRange> persistent_ranges_;
  uint64_t transient_used_[kFramesInFlight] = {};
  std::unordered_map<uint64_t, uint64_t> frame_ranges_;  // unregistered range key -> shared offset, this packet
  uint64_t frame_counter_ = 0;
  uint32_t next_descriptor_ = 1;
  std::vector<uint32_t> recycled_descriptors_;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderShader>> shaders_;
  // Draw pipelines by state hash; a slot is published once its compile finishes.
  struct PipelineSlot {
    std::unique_ptr<plume::RenderPipeline> pipeline;
    std::atomic<bool> ready{false};
    bool warm = false;  // queued by the start-up warm-up; counted in warm_pending_ until compiled
  };
  std::atomic<uint32_t> warm_pending_{0};
  std::unordered_map<uint64_t, std::unique_ptr<PipelineSlot>> draw_pipelines_;
  std::vector<std::thread> compile_threads_;
  std::mutex compile_mutex_;
  std::condition_variable compile_cv_;
  std::deque<std::pair<PipelineSlot*, plume::RenderGraphicsPipelineDesc>> compile_queue_;
  bool compile_stop_ = false;
  void CompileLoop();
  std::map<std::array<uint32_t, 5>, std::unique_ptr<plume::RenderFramebuffer>> draw_framebuffers_;

  // gpu_fps: the counter text rasterized from the game's font whenever the number changes.
  struct FpsOverlay {
    struct Glyph {
      int x = 0, y = 0, w = 0, h = 0, xo = 0, yo = 0, advance = 0;
    };
    bool tried = false;
    bool ok = false;
    std::unordered_map<uint32_t, Glyph> glyphs;
    std::vector<uint8_t> atlas;  // glyph coverage, one byte per texel
    int atlas_w = 0, atlas_h = 0, line = 0;
    std::string text;
    HostResource texture;
    std::chrono::steady_clock::time_point window_start{};
    uint32_t frames = 0;
    uint32_t shown = 0;
  } fps_;

  std::atomic<bool> resize_requested_{false};
};

std::mutex g_font_mutex;
std::filesystem::path g_font_root;

bool Backend::Create(rex::ui::Window* window) {
  auto* hwnd = static_cast<plume::RenderWindow>(window->GetNativeWindowHandle());
  if (!hwnd) {
    REXGPU_ERROR("[gpu] the window has no native handle");
    return false;
  }
  interface_ = plume::CreateD3D12Interface();
  if (!interface_) {
    REXGPU_ERROR("[gpu] cannot create the d3d12 interface");
    return false;
  }
  std::string adapter;
  const std::string wanted = REXCVAR_GET(gpu_adapter);
  if (!wanted.empty()) {
    for (const auto& name : interface_->getDeviceNames()) {
      if (name.find(wanted) != std::string::npos) {
        adapter = name;
        break;
      }
    }
    if (adapter.empty()) REXGPU_WARN("[gpu] no adapter matches '{}', using the default", wanted);
  }
  device_ = interface_->createDevice(adapter);
  if (!device_) {
    REXGPU_ERROR("[gpu] cannot create a device");
    return false;
  }
  REXGPU_INFO("[gpu] d3d12 on {}", device_->getDescription().name);

  queue_ = device_->createCommandQueue(plume::RenderCommandListType::DIRECT);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    lists_[i] = queue_->createCommandList();
    fences_[i] = device_->createCommandFence();
    acquire_semaphores_[i] = device_->createCommandSemaphore();
  }
  // One back buffer beyond the frames in flight, so acquiring never waits on scanout.
  plume::RenderSwapChainDesc swap_desc(hwnd, kSwapChainFormat, kFramesInFlight + 1, false, kFramesInFlight);
  swap_chain_ = queue_->createSwapChain(swap_desc);
  if (!swap_chain_) {
    REXGPU_ERROR("[gpu] cannot create the swap chain");
    return false;
  }
  swap_chain_->setVsyncEnabled(REXCVAR_GET(gpu_vsync));
  if (!BuildSwapChainTargets()) return false;

  plume::RenderSamplerDesc sampler;
  sampler.minFilter = sampler.magFilter = plume::RenderFilter::NEAREST;
  sampler.mipmapMode = plume::RenderMipmapMode::NEAREST;
  sampler.addressU = sampler.addressV = sampler.addressW = plume::RenderTextureAddressMode::CLAMP;
  point_sampler_ = device_->createSampler(sampler);
  sampler.minFilter = sampler.magFilter = plume::RenderFilter::LINEAR;
  linear_sampler_ = device_->createSampler(sampler);

  sample_set_builder_.begin();
  sample_set_builder_.addTexture(0);
  sample_set_builder_.addImmutableSampler(1, point_sampler_.get());
  sample_set_builder_.addImmutableSampler(2, linear_sampler_.get());
  sample_set_builder_.end();
  plume::RenderPipelineLayoutBuilder layout;
  layout.begin(false, false);
  layout.addDescriptorSet(sample_set_builder_);
  layout.addPushConstant(0, 1, sizeof(BlitConstants),
                         plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
  layout.end();
  layout_ = layout.create(device_.get());

  vs_ = device_->createShader(shaders::kBlit_vs_dxil, sizeof(shaders::kBlit_vs_dxil), "VsMain",
                              plume::RenderShaderFormat::DXIL);
  ps_ = device_->createShader(shaders::kBlit_ps_dxil, sizeof(shaders::kBlit_ps_dxil), "PsMain",
                              plume::RenderShaderFormat::DXIL);
  resolve_vs_ = device_->createShader(shaders::kResolve_vs_dxil, sizeof(shaders::kResolve_vs_dxil), "VsMain",
                                      plume::RenderShaderFormat::DXIL);
  resolve_ps_ = device_->createShader(shaders::kResolve_ps_dxil, sizeof(shaders::kResolve_ps_dxil), "PsMain",
                                      plume::RenderShaderFormat::DXIL);
  if (!layout_ || !vs_ || !ps_ || !resolve_vs_ || !resolve_ps_ || !BlitPipeline(kSwapChainFormat)) {
    REXGPU_ERROR("[gpu] cannot build the blit pipeline");
    return false;
  }
  if (!overlay_.Create(device_.get(), kSwapChainFormat)) REXGPU_WARN("[gpu] overlay pipeline unavailable; no ImGui overlay");

  scale_pct_ = uint32_t(cc2::display::ResolutionScale());
  shadow_scale_pct_ = uint32_t(cc2::display::ShadowScale());
  // The forced sample count must be supported by every format a render target can have, so passes stay compatible.
  const int32_t wanted_msaa = REXCVAR_GET(gpu_msaa);
  if (wanted_msaa > 1) {
    static constexpr HostFormat kTargetFormats[] = {HostFormat::kRGBA8,     HostFormat::kRGB10A2,
                                                    HostFormat::kRGBA16Float, HostFormat::kRG16Float,
                                                    HostFormat::kR32Float,  HostFormat::kRG32Float,
                                                    HostFormat::kDepth32FloatStencil8};
    for (uint32_t count = std::min<uint32_t>(uint32_t(wanted_msaa), 8); count > 1 && !msaa_; count >>= 1) {
      bool all = true;
      for (HostFormat f : kTargetFormats) all &= (device_->getSampleCountsSupported(ToPlume(f)) & count) != 0;
      if (all) msaa_ = count;
    }
    if (msaa_ != uint32_t(wanted_msaa)) {
      REXGPU_WARN("[gpu] gpu_msaa {} is not supported for every render-target format; using {}", wanted_msaa,
                  msaa_ ? msaa_ : 1);
    }
  }
  if (scale_pct_ != 100 || msaa_) REXGPU_INFO("[gpu] render targets at {}% with {}x MSAA", scale_pct_, msaa_ ? msaa_ : 1);
  draws_ready_ = CreateDrawResources();
  if (!draws_ready_) REXGPU_ERROR("[gpu] draw resources unavailable; draws are dropped");
  return true;
}

bool Backend::CreateDrawResources() {
  if (!LoadShaderPack()) return false;
  const PackAbi& abi = ShaderPackAbi();

  // The sampler table is only as large as the pack's shaders can index: they clamp the sampler index to
  // sampler_heap - 1, so a slot past it would read another state's address modes.
  sampler_capacity_ = std::clamp(abi.sampler_heap, 1u, kSamplerHeap);
  sampler_set_builder_.begin();
  sampler_set_builder_.addSampler(0, sampler_capacity_);
  sampler_set_builder_.end(true, sampler_capacity_);
  texture_set_builder_.begin();
  texture_set_builder_.addTexture(0, kTextureHeap);
  texture_set_builder_.end(true, kTextureHeap);

  plume::RenderPipelineLayoutBuilder layout;
  layout.begin(false, false);
  using V = plume::RenderShaderVisibility;
  using T = plume::RenderRootDescriptorType;
  layout.addRootDescriptor(0, 0, T::CONSTANT_BUFFER);
  layout.addRootDescriptor(1, 0, T::CONSTANT_BUFFER, V::VERTEX);
  layout.addRootDescriptor(1, 0, T::CONSTANT_BUFFER, V::PIXEL);
  layout.addRootDescriptor(2, 0, T::CONSTANT_BUFFER);
  layout.addRootDescriptor(3, 0, T::CONSTANT_BUFFER);
  layout.addRootDescriptor(4, 0, T::CONSTANT_BUFFER, V::VERTEX);
  layout.addRootDescriptor(4, 0, T::CONSTANT_BUFFER, V::PIXEL);
  layout.addRootDescriptor(0, 0, T::SHADER_RESOURCE);
  layout.addRootDescriptor(0, 0, T::UNORDERED_ACCESS);
  if (abi.vfetch_table) layout.addRootDescriptor(5, 0, T::CONSTANT_BUFFER, V::VERTEX);
  layout.addDescriptorSet(sampler_set_builder_);  // set 0: samplers, space0
  layout.addDescriptorSet(texture_set_builder_);  // set 1: 2D arrays, space1
  layout.addDescriptorSet(texture_set_builder_);  // set 2: 3D, space2
  layout.addDescriptorSet(texture_set_builder_);  // set 3: cubes, space3
  layout.end();
  draw_layout_ = layout.create(device_.get());
  sampler_set_ = sampler_set_builder_.create(device_.get());
  texture_set_ = texture_set_builder_.create(device_.get());
  if (!draw_layout_ || !sampler_set_ || !texture_set_) return false;
  WarmPipelines();

  // The sampler table fills on demand, one slot per distinct SamplerBinding::state.
  sampler_slots_.clear();
  sampler_slot_count_ = 0;
  sampler_slot_state_.assign(sampler_capacity_, 0);
  sampler_slot_frame_.assign(sampler_capacity_, 0);

  // Blanks: every descriptor slot starts as a 2D blank, the clamp targets included.
  blank_2d_ = device_->createTexture(plume::RenderTextureDesc::Texture2D(1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM));
  blank_cube_ = device_->createTexture(plume::RenderTextureDesc::Texture(
      plume::RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6, plume::RenderFormat::R8G8B8A8_UNORM,
      plume::RenderTextureFlag::CUBE));
  blank_3d_ = device_->createTexture(plume::RenderTextureDesc::Texture3D(1, 1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM));
  if (!blank_2d_ || !blank_cube_ || !blank_3d_) return false;
  blank_2d_view_ = blank_2d_->createTextureView(plume::RenderTextureViewDesc::Texture2DArray(plume::RenderFormat::R8G8B8A8_UNORM));
  plume::RenderTextureViewDesc cube_view;
  cube_view.format = plume::RenderFormat::R8G8B8A8_UNORM;
  cube_view.dimension = plume::RenderTextureViewDimension::TEXTURE_CUBE;
  blank_cube_view_ = blank_cube_->createTextureView(cube_view);
  blank_3d_view_ = blank_3d_->createTextureView(plume::RenderTextureViewDesc::Texture3D(plume::RenderFormat::R8G8B8A8_UNORM));
  for (uint32_t i = 0; i < kTextureHeap; ++i) {
    texture_set_->setTexture(i, blank_2d_.get(), plume::RenderTextureLayout::SHADER_READ, blank_2d_view_.get());
  }
  texture_set_->setTexture(kBlankCube, blank_cube_.get(), plume::RenderTextureLayout::SHADER_READ, blank_cube_view_.get());
  texture_set_->setTexture(kBlank3D, blank_3d_.get(), plume::RenderTextureLayout::SHADER_READ, blank_3d_view_.get());

  const plume::RenderBufferFlags shared_flags = plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS |
                                                plume::RenderBufferFlag::INDEX;
  shared_memory_ = device_->createBuffer(plume::RenderBufferDesc::DefaultBuffer(kSharedMemorySize, shared_flags));
  dummy_uav_ = device_->createBuffer(plume::RenderBufferDesc::DefaultBuffer(256, plume::RenderBufferFlag::UNORDERED_ACCESS));
  if (!shared_memory_ || !dummy_uav_) return false;
  persistent_.Reset(kPersistentSize);

  // The blanks and the shared buffer need their layouts set once.
  BeginList();
  auto* list = lists_[slot_].get();
  const plume::RenderTextureBarrier blanks[] = {
      plume::RenderTextureBarrier(blank_2d_.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(blank_cube_.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(blank_3d_.get(), plume::RenderTextureLayout::SHADER_READ),
  };
  list->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE, blanks, 3);
  list->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE,
                 plume::RenderBufferBarrier(shared_memory_.get(), plume::RenderBufferAccess::READ));
  SubmitAndWait();
  REXGPU_DEBUG("[gpu] draw path ready: {} (b0 {} bytes, vfetch table {})", ShaderPackName(), abi.system_size,
              abi.vfetch_table);
  return true;
}

bool Backend::BuildSwapChainTargets() {
  swap_framebuffers_.clear();
  render_semaphores_.clear();
  if (swap_chain_->isEmpty()) return true;
  for (uint32_t i = 0; i < swap_chain_->getTextureCount(); ++i) {
    const plume::RenderTexture* attachments[1] = {swap_chain_->getTexture(i)};
    auto framebuffer = device_->createFramebuffer(plume::RenderFramebufferDesc(attachments, 1));
    auto semaphore = device_->createCommandSemaphore();
    if (!framebuffer || !semaphore) {
      REXGPU_ERROR("[gpu] cannot build back buffer {}", i);
      swap_framebuffers_.clear();
      render_semaphores_.clear();
      return false;
    }
    swap_framebuffers_.push_back(std::move(framebuffer));
    render_semaphores_.push_back(std::move(semaphore));
  }
  return true;
}

void Backend::WaitAll() {
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    if (submitted_[i]) {
      queue_->waitForCommandFence(fences_[i].get());
      submitted_[i] = false;
    }
  }
}

void Backend::RebuildSwapChain() {
  SubmitAndWait();
  WaitAll();
  swap_framebuffers_.clear();
  render_semaphores_.clear();
  if (!swap_chain_->resize() || !BuildSwapChainTargets()) {
    if (swap_chain_->getWidth() && swap_chain_->getHeight()) REXGPU_ERROR("[gpu] swap chain resize failed");
  } else {
    REXGPU_DEBUG("[gpu] swap chain {}x{}", swap_chain_->getWidth(), swap_chain_->getHeight());
  }
}

void Backend::Destroy() {
  if (queue_) {
    if (list_open_) SubmitAndWait();
    WaitAll();
  }
  overlay_.Destroy();
  if (!compile_threads_.empty()) {
    {
      std::lock_guard lock(compile_mutex_);
      compile_stop_ = true;
    }
    compile_cv_.notify_all();
    for (auto& thread : compile_threads_) thread.join();
    compile_threads_.clear();
  }
  draw_framebuffers_.clear();
  resources_.clear();
  resize_pipelines_.clear();
  for (auto& shader : resize_shaders_) shader.reset();
  draw_pipelines_.clear();
  shaders_.clear();
  for (auto& chunks : uploads_) chunks.clear();
  shared_memory_.reset();
  dummy_uav_.reset();
  blank_2d_view_.reset();
  blank_cube_view_.reset();
  blank_3d_view_.reset();
  blank_2d_.reset();
  blank_cube_.reset();
  blank_3d_.reset();
  sampler_slots_.clear();
  sampler_slot_count_ = 0;
  sampler_objects_.clear();
  samplers_.clear();
  texture_set_.reset();
  sampler_set_.reset();
  draw_layout_.reset();
  pipelines_.clear();
  resolve_pipelines_.clear();
  resolve_ps_.reset();
  resolve_vs_.reset();
  ps_.reset();
  vs_.reset();
  layout_.reset();
  linear_sampler_.reset();
  point_sampler_.reset();
  swap_framebuffers_.clear();
  render_semaphores_.clear();
  swap_chain_.reset();
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    acquire_semaphores_[i].reset();
    fences_[i].reset();
    lists_[i].reset();
  }
  queue_.reset();
  device_.reset();
  interface_.reset();
}

void Backend::ResetUploads(uint32_t slot) {
  for (auto& chunk : uploads_[slot]) chunk.used = 0;
  constant_uploads_.clear();
  transient_used_[slot] = 0;
}

// Slot 0 grows up from the bottom of the transient area and slot 1 down from the top. When the frame outgrows what
// the other slot holds, it waits for that slot's fence and takes the whole area.
bool Backend::AllocateTransient(uint64_t size, uint64_t& offset) {
  const uint64_t aligned = (size + 255) & ~255ull;
  const uint32_t other = slot_ ^ 1;
  if (transient_used_[slot_] + transient_used_[other] + aligned > kTransientSize && submitted_[other]) {
    queue_->waitForCommandFence(fences_[other].get());
    submitted_[other] = false;
    ResetUploads(other);
  }
  if (transient_used_[slot_] + transient_used_[other] + aligned > kTransientSize) return false;
  transient_used_[slot_] += aligned;
  offset = slot_ == 0 ? kPersistentSize + transient_used_[slot_] - aligned : kSharedMemorySize - transient_used_[slot_];
  return true;
}

void Backend::BeginList() {
  if (list_open_) return;
  if (submitted_[slot_]) {
    queue_->waitForCommandFence(fences_[slot_].get());
    submitted_[slot_] = false;
    ResetUploads(slot_);
  }
  lists_[slot_]->begin();
  list_open_ = true;
  bound_ = nullptr;
}

// Closes the open list, runs it and waits, leaving the slot free for the next BeginList.
void Backend::SubmitAndWait() {
  if (!list_open_) return;
  Unbind();
  lists_[slot_]->end();
  queue_->executeCommandLists(lists_[slot_].get(), fences_[slot_].get());
  queue_->waitForCommandFence(fences_[slot_].get());
  list_open_ = false;
}

void Backend::Unbind() {
  if (bound_) {
    lists_[slot_]->setFramebuffer(nullptr);
    bound_ = nullptr;
  }
}

void Backend::Transition(HostResource& resource, plume::RenderTextureLayout layout) {
  if (resource.layout == layout) return;
  resource.layout = layout;
  pending_barriers_.emplace_back(resource.texture.get(), layout);
}

// A pending barrier batch ends the bound render pass first.
void Backend::FlushTransitions() {
  if (pending_barriers_.empty()) return;
  Unbind();
  lists_[slot_]->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE, pending_barriers_.data(),
                          uint32_t(pending_barriers_.size()));
  pending_barriers_.clear();
}

void Backend::BindTarget(HostResource& resource) {
  auto* list = lists_[slot_].get();
  if (bound_ == resource.framebuffer.get() && resource.layout == (resource.depth()
                                                                        ? plume::RenderTextureLayout::DEPTH_WRITE
                                                                        : plume::RenderTextureLayout::COLOR_WRITE)) {
    return;
  }
  Transition(resource, resource.depth() ? plume::RenderTextureLayout::DEPTH_WRITE : plume::RenderTextureLayout::COLOR_WRITE);
  FlushTransitions();
  Unbind();
  list->setFramebuffer(resource.framebuffer.get());
  bound_ = resource.framebuffer.get();
}

HostResource* Backend::Get(const ResourceDesc& desc) {
  if (desc.id == kNoResource || !desc.width || !desc.height) return nullptr;
  auto it = resources_.find(desc.id);
  if (it != resources_.end()) return &it->second;
  HostResource resource;
  resource.desc = desc;
  resource.format = ToPlume(desc.format);
  // Render targets and resolve destinations follow the resolution scale, the course shadow map its own; decoded
  // textures keep their size.
  const bool scaled = !resource.sampled_only();
  resource.scale = scaled ? ScaleFor(desc) : 100;
  resource.width = uint32_t(std::max(Scaled(resource, int32_t(desc.width)), 1));
  resource.height = uint32_t(std::max(Scaled(resource, int32_t(desc.height)), 1));
  const bool target = desc.kind == ResourceKind::kColorTarget || desc.kind == ResourceKind::kDepthTarget;
  resource.samples = target ? HostSamples(desc) : 1;
  const plume::RenderMultisampling multisampling(plume::RenderSampleCounts(resource.samples));
  const plume::RenderTexture* attachments[1] = {nullptr};
  if (resource.sampled_only()) {
    if (resource.volume()) {
      resource.texture = device_->createTexture(plume::RenderTextureDesc::Texture3D(
          desc.width, desc.height, std::max(desc.depth, 1u), resource.mips(), resource.format));
    } else {
      resource.texture = device_->createTexture(plume::RenderTextureDesc::Texture(
          plume::RenderTextureDimension::TEXTURE_2D, desc.width, desc.height, 1, resource.mips(), resource.layers(),
          resource.format, resource.cube() ? plume::RenderTextureFlag::CUBE : 0u));
    }
  } else if (resource.depth()) {
    resource.texture = device_->createTexture(
        plume::RenderTextureDesc::DepthTarget(resource.width, resource.height, resource.format, multisampling));
    if (resource.texture) {
      resource.framebuffer =
          device_->createFramebuffer(plume::RenderFramebufferDesc(nullptr, 0, resource.texture.get()));
    }
  } else {
    resource.texture = device_->createTexture(
        plume::RenderTextureDesc::ColorTarget(resource.width, resource.height, resource.format, multisampling));
    if (resource.texture) {
      attachments[0] = resource.texture.get();
      resource.framebuffer = device_->createFramebuffer(plume::RenderFramebufferDesc(attachments, 1));
    }
  }
  if (!resource.texture || (!resource.sampled_only() && !resource.framebuffer)) {
    REXGPU_ERROR("[gpu] cannot create host resource {} ({}x{} {})", desc.id, desc.width, desc.height,
                 HostFormatName(desc.format));
    return nullptr;
  }
  auto& stored = resources_.emplace(desc.id, std::move(resource)).first->second;
  if (stored.sampled_only()) return &stored;
  // New targets start defined: black, depth 1, or magenta for a format the renderer cannot represent.
  BeginList();
  BindTarget(stored);
  if (stored.depth()) {
    lists_[slot_]->clearDepthStencil(true, true, 1.0f, 0);
  } else if (desc.format == HostFormat::kUnsupported) {
    lists_[slot_]->clearColor(0, plume::RenderColor(1.0f, 0.0f, 1.0f, 1.0f));
  } else {
    lists_[slot_]->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  return &stored;
}

plume::RenderPipeline* Backend::BlitPipeline(plume::RenderFormat format, bool blend) {
  auto& pipeline = pipelines_[uint32_t(format) | (blend ? 0x10000u : 0u)];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout_.get();
    desc.vertexShader = vs_.get();
    desc.pixelShader = ps_.get();
    desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = format;
    desc.renderTargetBlend[0] = blend ? plume::RenderBlendDesc::AlphaBlend() : plume::RenderBlendDesc::Copy();
    pipeline = device_->createGraphicsPipeline(desc);
    if (!pipeline) REXGPU_ERROR("[gpu] cannot create a blit pipeline for host format {}", uint32_t(format));
  }
  return pipeline.get();
}

plume::RenderDescriptorSet* Backend::SampleSet(HostResource& resource) {
  if (!resource.sample_set) {
    resource.sample_set = sample_set_builder_.create(device_.get());
    if (resource.sample_set && resource.depth()) {
      // A depth-stencil texture needs an explicit view that picks its depth plane.
      auto view = resource.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(resource.format));
      resource.sample_set->setTexture(0, resource.texture.get(), plume::RenderTextureLayout::SHADER_READ, view.get());
      resource.view_objects.push_back(std::move(view));
    } else if (resource.sample_set) {
      resource.sample_set->setTexture(0, resource.texture.get(), plume::RenderTextureLayout::SHADER_READ);
    }
  }
  return resource.sample_set.get();
}

void Backend::Blit(HostResource& source, const plume::RenderRect& source_rect, plume::RenderTexture* target,
                   plume::RenderFramebuffer* framebuffer, plume::RenderFormat target_format,
                   const plume::RenderRect& dest_rect, float scale, bool linear, bool blend, HostResource* owner) {
  auto* list = lists_[slot_].get();
  auto* pipeline = BlitPipeline(target_format, blend);
  auto* set = SampleSet(source);
  if (!pipeline || !set) return;
  Transition(source, plume::RenderTextureLayout::SHADER_READ);
  // Callers name the owning resource when the target is one, so a blit never scans every texture in resources_.
  if (owner) {
    Transition(*owner, plume::RenderTextureLayout::COLOR_WRITE);
  } else {
    pending_barriers_.emplace_back(target, plume::RenderTextureLayout::COLOR_WRITE);
  }
  FlushTransitions();
  Unbind();
  list->setFramebuffer(framebuffer);
  bound_ = framebuffer;
  list->setGraphicsPipelineLayout(layout_.get());
  list->setPipeline(pipeline);
  list->setGraphicsDescriptorSet(set, 0);
  const float sw = float(source.width);
  const float sh = float(source.height);
  BlitConstants constants{};
  constants.uv_offset[0] = float(source_rect.left) / sw;
  constants.uv_offset[1] = float(source_rect.top) / sh;
  constants.uv_scale[0] = float(source_rect.right - source_rect.left) / sw;
  constants.uv_scale[1] = float(source_rect.bottom - source_rect.top) / sh;
  constants.scale[0] = constants.scale[1] = constants.scale[2] = constants.scale[3] = scale;
  constants.linear_filter = linear ? 1 : 0;
  list->setGraphicsPushConstants(0, &constants, 0, sizeof(constants));
  list->setViewports(plume::RenderViewport(float(dest_rect.left), float(dest_rect.top),
                                           float(dest_rect.right - dest_rect.left),
                                           float(dest_rect.bottom - dest_rect.top)));
  list->setScissors(dest_rect);
  list->drawInstanced(3, 1, 0, 0);
}

void Backend::Execute(Record& record) {
  std::visit([this](auto& r) { Execute(r); }, record);
}

// Guest rects to host texels of the target, clipped to it.
std::vector<plume::RenderRect> Backend::ClipRects(const std::vector<Rect>& rects, const HostResource& target) const {
  std::vector<plume::RenderRect> out;
  for (const Rect& r : rects) {
    const int32_t l = std::max(Scaled(target, r.left), 0), t = std::max(Scaled(target, r.top), 0);
    const int32_t rr = std::min(Scaled(target, r.right), int32_t(target.width));
    const int32_t b = std::min(Scaled(target, r.bottom), int32_t(target.height));
    if (rr > l && b > t) out.emplace_back(l, t, rr, b);
  }
  return out;
}

uint32_t Backend::HostSamples(const ResourceDesc& desc) const {
  return std::max<uint32_t>(desc.samples, msaa_ ? msaa_ : 1);
}

plume::RenderPipeline* Backend::ResolvePipeline(plume::RenderFormat format) {
  auto& pipeline = resolve_pipelines_[uint32_t(format)];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout_.get();
    desc.vertexShader = resolve_vs_.get();
    desc.pixelShader = resolve_ps_.get();
    desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    pipeline = device_->createGraphicsPipeline(desc);
    if (!pipeline) REXGPU_ERROR("[gpu] cannot create a resolve pipeline for host format {}", uint32_t(format));
  }
  return pipeline.get();
}

// Shader resolve of a multisampled target: colour averages its samples, depth keeps sample 0, texel for texel.
void Backend::ResolveSamples(HostResource& source, int32_t sx, int32_t sy, HostResource& dest,
                             const plume::RenderRect& dest_rect, float scale) {
  auto* list = lists_[slot_].get();
  auto* pipeline = ResolvePipeline(dest.format);
  auto* set = SampleSet(source);
  if (!pipeline || !set || !dest.framebuffer) return;
  Transition(source, plume::RenderTextureLayout::SHADER_READ);
  Transition(dest, plume::RenderTextureLayout::COLOR_WRITE);
  FlushTransitions();
  Unbind();
  list->setFramebuffer(dest.framebuffer.get());
  bound_ = dest.framebuffer.get();
  list->setGraphicsPipelineLayout(layout_.get());
  list->setPipeline(pipeline);
  list->setGraphicsDescriptorSet(set, 0);
  ResolveConstants constants{};
  constants.source_offset[0] = sx - dest_rect.left;
  constants.source_offset[1] = sy - dest_rect.top;
  constants.samples = source.samples;
  constants.average = source.depth() ? 0 : 1;
  constants.scale[0] = constants.scale[1] = constants.scale[2] = constants.scale[3] = scale;
  list->setGraphicsPushConstants(0, &constants, 0, sizeof(constants));
  list->setViewports(plume::RenderViewport(float(dest_rect.left), float(dest_rect.top),
                                           float(dest_rect.right - dest_rect.left),
                                           float(dest_rect.bottom - dest_rect.top)));
  list->setScissors(dest_rect);
  list->drawInstanced(3, 1, 0, 0);
}

void Backend::Execute(const ClearRecord& r) {
  BeginList();
  auto* list = lists_[slot_].get();
  for (const ResourceDesc& desc : r.color) {
    HostResource* target = Get(desc);
    if (!target || desc.format == HostFormat::kUnsupported) continue;
    const auto rects = ClipRects(r.rects, *target);
    if (!r.rects.empty() && rects.empty()) continue;
    BindTarget(*target);
    list->clearColor(0, plume::RenderColor(r.color_value[0], r.color_value[1], r.color_value[2], r.color_value[3]),
                     rects.empty() ? nullptr : rects.data(), uint32_t(rects.size()));
  }
  if (HostResource* depth = Get(r.depth); depth && depth->desc.format != HostFormat::kUnsupported) {
    const auto rects = ClipRects(r.rects, *depth);
    if (r.rects.empty() || !rects.empty()) {
      BindTarget(*depth);
      list->clearDepthStencil(r.clear_depth, r.clear_stencil, r.depth_value, r.stencil_value,
                              rects.empty() ? nullptr : rects.data(), uint32_t(rects.size()));
    }
  }
}

void Backend::Execute(const ResolveRecord& r) {
  BeginList();
  auto* list = lists_[slot_].get();
  HostResource* source = Get(r.source);
  HostResource* dest = Get(r.dest);
  if (!source || !dest) return;
  if (source->depth() && r.dest.format != HostFormat::kR32Float) {
    // Only depth into a float depth texture is modelled; any other destination shows magenta.
    BindTarget(*dest);
    list->clearColor(0, plume::RenderColor(1.0f, 0.0f, 1.0f, 1.0f));
  } else if (r.dest.format != HostFormat::kUnsupported) {
    // Guest rects in host texels; source and destination share their scale, so the copy stays 1:1.
    int32_t sx = Scaled(*source, r.source_rect.left), sy = Scaled(*source, r.source_rect.top);
    int32_t dx = Scaled(*dest, r.dest_x), dy = Scaled(*dest, r.dest_y);
    const int32_t right = std::min(Scaled(*source, r.source_rect.right), int32_t(source->width));
    const int32_t bottom = std::min(Scaled(*source, r.source_rect.bottom), int32_t(source->height));
    if (dx < 0) sx -= dx, dx = 0;
    if (dy < 0) sy -= dy, dy = 0;
    const int32_t w = std::min(right - sx, int32_t(dest->width) - dx);
    const int32_t h = std::min(bottom - sy, int32_t(dest->height) - dy);
    if (w > 0 && h > 0 && source->samples > 1) {
      ResolveSamples(*source, sx, sy, *dest, plume::RenderRect(dx, dy, dx + w, dy + h), r.scale);
    } else if (w > 0 && h > 0) {
      if (source->format == dest->format && r.scale == 1.0f && !source->depth()) {
        Transition(*source, plume::RenderTextureLayout::COPY_SOURCE);
        Transition(*dest, plume::RenderTextureLayout::COPY_DEST);
        FlushTransitions();
        Unbind();
        const plume::RenderBox box(sx, sy, sx + w, sy + h);
        list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(dest->texture.get()),
                                plume::RenderTextureCopyLocation::Subresource(source->texture.get()), uint32_t(dx),
                                uint32_t(dy), 0, &box);
      } else {
        Blit(*source, plume::RenderRect(sx, sy, sx + w, sy + h), dest->texture.get(), dest->framebuffer.get(),
             dest->format, plume::RenderRect(dx, dy, dx + w, dy + h), r.scale, false, false, dest);
      }
    }
  }
  // The clear covers the resolved rect only: a tiled target resolves band by band, and a whole-surface clear on the
  // first band would wipe the bands still to be resolved.
  const plume::RenderRect cleared(Scaled(*source, r.source_rect.left), Scaled(*source, r.source_rect.top),
                                  std::min(Scaled(*source, r.source_rect.right), int32_t(source->width)),
                                  std::min(Scaled(*source, r.source_rect.bottom), int32_t(source->height)));
  const bool has_rect = cleared.right > cleared.left && cleared.bottom > cleared.top;
  if (r.clear_color && has_rect && source->desc.format != HostFormat::kUnsupported && !source->depth()) {
    BindTarget(*source);
    list->clearColor(0, plume::RenderColor(r.clear_color_value[0], r.clear_color_value[1], r.clear_color_value[2],
                                           r.clear_color_value[3]), &cleared, 1);
  }
  if (HostResource* depth = Get(r.clear_depth_target); depth && depth->desc.format != HostFormat::kUnsupported) {
    const plume::RenderRect depth_rect(cleared.left, cleared.top, std::min(cleared.right, int32_t(depth->width)),
                                       std::min(cleared.bottom, int32_t(depth->height)));
    const bool depth_has_rect = depth_rect.right > depth_rect.left && depth_rect.bottom > depth_rect.top;
    BindTarget(*depth);
    list->clearDepthStencil(true, true, r.clear_z, r.clear_stencil, depth_has_rect ? &depth_rect : nullptr,
                            depth_has_rect ? 1 : 0);
  }
}

void Backend::Execute(const SwapRecord& r) {
  BeginList();
  HostResource* front = Get(r.front);
  if (resize_requested_.exchange(false, std::memory_order_acq_rel) || swap_chain_->needsResize()) {
    RebuildSwapChain();
    BeginList();
  }
  auto* list = lists_[slot_].get();
  uint32_t index = 0;
  bool presentable = !swap_framebuffers_.empty() &&
                     swap_chain_->acquireTexture(acquire_semaphores_[slot_].get(), &index);
  if (!presentable && !swap_framebuffers_.empty()) {
    RebuildSwapChain();
    BeginList();
    list = lists_[slot_].get();
    presentable = !swap_framebuffers_.empty() &&
                  swap_chain_->acquireTexture(acquire_semaphores_[slot_].get(), &index);
  }
  if (presentable) {
    plume::RenderTexture* back = swap_chain_->getTexture(index);
    plume::RenderFramebuffer* back_framebuffer = swap_framebuffers_[index].get();
    const uint32_t bw = swap_chain_->getWidth();
    const uint32_t bh = swap_chain_->getHeight();
    if (cc2::display::FpsCounter()) UpdateFps();
    const bool pointer = g_pointer_visible.load(std::memory_order_acquire) && REXCVAR_GET(gpu_pointer) != "off";
    if (pointer) UpdatePointer();
    FlushTransitions();
    Unbind();
    const OverlayAllocator overlay_uploads = OverlayUploads();
    const bool overlay = overlay_.Prepare(list, slot_, overlay_uploads);
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(back, plume::RenderTextureLayout::COLOR_WRITE));
    list->setFramebuffer(back_framebuffer);
    bound_ = back_framebuffer;
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    // The pointer goes into the game's own frame at internal resolution, so it scales with the res scale
    // like the menu it points at.
    if (front && pointer && front->framebuffer) {
      DrawPointer(front->texture.get(), front->framebuffer.get(), front->width, front->height, front);
    }
    if (front) {
      plume::RenderRect fit;
      FitRect(bw, bh, uint32_t(cc2::display::AspectRatio() * 9000.0f + 0.5f), 9000, fit);
      Blit(*front, plume::RenderRect(0, 0, int32_t(front->width), int32_t(front->height)), back, back_framebuffer,
           kSwapChainFormat, fit, 1.0f, true);
    }
    if (cc2::display::FpsCounter()) DrawFps(back, back_framebuffer, bw, bh);
    if (pointer && !(front && front->framebuffer)) DrawPointer(back, back_framebuffer, bw, bh, nullptr);
    if (overlay) {
      if (bound_ != back_framebuffer) {
        list->setFramebuffer(back_framebuffer);
        bound_ = back_framebuffer;
      }
      overlay_.Draw(list, bw, bh, overlay_uploads);
    }
    Unbind();
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
    list->end();
    const plume::RenderCommandList* lists[] = {list};
    plume::RenderCommandSemaphore* waits[] = {acquire_semaphores_[slot_].get()};
    plume::RenderCommandSemaphore* signals[] = {render_semaphores_[index].get()};
    queue_->executeCommandLists(lists, 1, waits, 1, signals, 1, fences_[slot_].get());
    const int vsync = g_vsync_request.exchange(-1, std::memory_order_acq_rel);
    if (vsync >= 0) swap_chain_->setVsyncEnabled(vsync != 0);
    if (!swap_chain_->present(index, signals, 1)) resize_requested_.store(true, std::memory_order_release);
  } else {
    FlushTransitions();
    Unbind();
    list->end();
    queue_->executeCommandLists(list, fences_[slot_].get());
  }
  submitted_[slot_] = true;
  list_open_ = false;
  slot_ = (slot_ + 1) % kFramesInFlight;
  NotifyOverlayPresented();
  ++frame_counter_;
  // A frame's persistent frees are safe once every slot has cycled past it.
  if (frame_counter_ > kFramesInFlight) persistent_.Retire(frame_counter_ - kFramesInFlight - 1);
  ResizeTargets(uint32_t(cc2::display::ResolutionScale()), uint32_t(cc2::display::ShadowScale()));
}

// Counts presents per half second and, when the number changes, rasterizes "<n> FPS" from cardigan_bd: the .fnt
// metrics and the DXT5 atlas page the digits and "FPS" live on (page 1), whose alpha is the glyph coverage.
void Backend::UpdateFps() {
  auto& f = fps_;
  const auto now = std::chrono::steady_clock::now();
  if (f.window_start == std::chrono::steady_clock::time_point{}) f.window_start = now;
  ++f.frames;
  const double elapsed = std::chrono::duration<double>(now - f.window_start).count();
  if (elapsed >= 0.5) {
    f.shown = uint32_t(f.frames / elapsed + 0.5);
    f.frames = 0;
    f.window_start = now;
  }
  if (!f.tried) {
    std::filesystem::path root;
    {
      std::lock_guard lock(g_font_mutex);
      root = g_font_root;
    }
    // The app names the folder once the game data root is known; until then there is nothing to load.
    if (root.empty()) return;
    f.tried = true;
    std::ifstream fnt(root / "cardigan_bd.fnt");
    std::ifstream dds(root / "cardigan_bd_1.dds", std::ios::binary);
    if (!fnt || !dds) {
      REXGPU_WARN("[gpu] gpu_fps: font not found under {}", root.string());
      return;
    }
    std::string line;
    while (std::getline(fnt, line)) {
      std::istringstream in(line);
      std::string tag;
      in >> tag;
      std::unordered_map<std::string, int> kv;
      for (std::string item; in >> item;) {
        const auto eq = item.find('=');
        if (eq != std::string::npos && eq + 1 < item.size() && item[eq + 1] != '"') {
          kv[item.substr(0, eq)] = std::atoi(item.c_str() + eq + 1);
        }
      }
      if (tag == "common") f.line = kv["lineHeight"];
      if (tag == "char" && kv["page"] == 1) {
        f.glyphs[uint32_t(kv["id"])] = {kv["x"], kv["y"], kv["width"], kv["height"], kv["xoffset"], kv["yoffset"],
                                        kv["xadvance"]};
      }
      // The space's box is on page 0, so only its advance is used.
      if (tag == "char" && kv["id"] == 32) f.glyphs[32] = {0, 0, 0, 0, 0, 0, kv["xadvance"]};
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(dds)), std::istreambuf_iterator<char>());
    if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4) != 0) return;
    std::memcpy(&f.atlas_h, &file[12], 4);
    std::memcpy(&f.atlas_w, &file[16], 4);
    const int bx = f.atlas_w / 4, by = f.atlas_h / 4;
    if (file.size() < 128 + size_t(bx) * by * 16) return;
    f.atlas.assign(size_t(f.atlas_w) * f.atlas_h, 0);
    uint8_t tile[16][4];
    for (int y = 0; y < by; ++y) {
      for (int x = 0; x < bx; ++x) {
        DecodeBlockRGBA(HostFormat::kBC3, &file[128 + (size_t(y) * bx + x) * 16], tile);
        for (int i = 0; i < 16; ++i) f.atlas[size_t(y * 4 + i / 4) * f.atlas_w + x * 4 + i % 4] = tile[i][3];
      }
    }
    f.ok = true;
  }
  const std::string text = std::to_string(f.shown) + " FPS";
  if (!f.ok || text == f.text) return;
  f.text = text;
  // White glyph cores over a black outline: the coverage dilated by a small disk.
  constexpr int kPad = 4, kRadius = 3;
  int width = 2 * kPad;
  for (char c : text) {
    auto it = f.glyphs.find(uint8_t(c));
    if (it != f.glyphs.end()) width += it->second.advance;
  }
  const int height = f.line + 2 * kPad;
  std::vector<uint8_t> core(size_t(width) * height, 0), outline(core.size(), 0);
  int pen = kPad;
  for (char c : text) {
    auto it = f.glyphs.find(uint8_t(c));
    if (it == f.glyphs.end()) continue;
    const auto& g = it->second;
    for (int y = 0; y < g.h; ++y) {
      for (int x = 0; x < g.w; ++x) {
        const int dx = pen + g.xo + x, dy = kPad + g.yo + y;
        if (dx < 0 || dy < 0 || dx >= width || dy >= height || g.x + x >= f.atlas_w || g.y + y >= f.atlas_h) continue;
        auto& d = core[size_t(dy) * width + dx];
        d = std::max(d, f.atlas[size_t(g.y + y) * f.atlas_w + g.x + x]);
      }
    }
    pen += g.advance;
  }
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      uint8_t m = 0;
      for (int oy = -kRadius; oy <= kRadius; ++oy) {
        for (int ox = -kRadius; ox <= kRadius; ++ox) {
          const int sx = x + ox, sy = y + oy;
          if (ox * ox + oy * oy > kRadius * kRadius || sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
          m = std::max(m, core[size_t(sy) * width + sx]);
        }
      }
      outline[size_t(y) * width + x] = m;
    }
  }
  if (!f.texture.texture || f.texture.desc.width != uint32_t(width) || f.texture.desc.height != uint32_t(height)) {
    f.texture = HostResource{};
    f.texture.desc.width = f.texture.width = uint32_t(width);
    f.texture.desc.height = f.texture.height = uint32_t(height);
    f.texture.desc.kind = ResourceKind::kSampledTexture;
    f.texture.format = plume::RenderFormat::R8G8B8A8_UNORM;
    f.texture.texture = device_->createTexture(
        plume::RenderTextureDesc::Texture2D(uint32_t(width), uint32_t(height), 1, f.texture.format));
    if (!f.texture.texture) return;
  }
  const uint32_t pitch = (uint32_t(width) * 4 + 255) & ~255u;
  UploadSpan span = Upload(uint64_t(pitch) * height, 512);
  if (!span.data) return;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const uint8_t c = core[size_t(y) * width + x], o = outline[size_t(y) * width + x];
      uint8_t* p = span.data + size_t(y) * pitch + x * 4;
      const uint8_t v = o ? uint8_t(std::min(255, c * 255 / o)) : 0;
      p[0] = p[1] = p[2] = v;
      p[3] = o;
    }
  }
  Transition(f.texture, plume::RenderTextureLayout::COPY_DEST);
  FlushTransitions();
  lists_[slot_]->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(f.texture.texture.get()),
                                   plume::RenderTextureCopyLocation::PlacedFootprint(
                                       span.buffer, f.texture.format, uint32_t(width), uint32_t(height), 1, pitch / 4,
                                       span.offset));
  Transition(f.texture, plume::RenderTextureLayout::SHADER_READ);
}

// Composites the counter into the top right of the back buffer, at half the font's size on a 1440-line output.
void Backend::DrawFps(plume::RenderTexture* back, plume::RenderFramebuffer* framebuffer, uint32_t width,
                      uint32_t height) {
  auto& t = fps_.texture;
  if (!fps_.ok || !t.texture) return;
  const float scale = std::max(0.25f, float(height) / 1440.0f);
  const int32_t w = int32_t(t.desc.width * scale), h = int32_t(t.desc.height * scale);
  const int32_t margin = int32_t(24 * scale);
  const plume::RenderRect dest(int32_t(width) - w - margin, margin, int32_t(width) - margin, margin + h);
  Blit(t, plume::RenderRect(0, 0, int32_t(t.desc.width), int32_t(t.desc.height)), back, framebuffer, kSwapChainFormat,
       dest, 1.0f, true, true);
}

// Bakes and uploads the pointer sprite the first time the pointer shows; the assets sit beside the font folder.
void Backend::UpdatePointer() {
  auto& p = pointer_;
  if (p.tried) return;
  std::filesystem::path root;
  {
    std::lock_guard lock(g_font_mutex);
    root = g_font_root;
  }
  if (root.empty()) return;
  p.tried = true;
  const bool loaded = REXCVAR_GET(gpu_pointer) == "model"
                          ? BakePointerSprite(root.parent_path(), 256, float(REXCVAR_GET(gpu_pointer_angle)), p.sprite)
                          : LoadSelectorSprite(root.parent_path(), float(REXCVAR_GET(gpu_pointer_angle)), p.sprite);
  if (!loaded) {
    REXGPU_WARN("[gpu] pointer: hand assets not found under {}", root.parent_path().string());
    return;
  }
  const uint32_t width = p.sprite.width, height = p.sprite.height;
  p.texture = HostResource{};
  p.texture.desc.width = p.texture.width = width;
  p.texture.desc.height = p.texture.height = height;
  p.texture.desc.kind = ResourceKind::kSampledTexture;
  p.texture.format = plume::RenderFormat::R8G8B8A8_UNORM;
  p.texture.texture = device_->createTexture(plume::RenderTextureDesc::Texture2D(width, height, 1, p.texture.format));
  if (!p.texture.texture) return;
  const uint32_t pitch = (width * 4 + 255) & ~255u;
  UploadSpan span = Upload(uint64_t(pitch) * height, 512);
  if (!span.data) return;
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(span.data + size_t(y) * pitch, &p.sprite.rgba[size_t(y) * width * 4], size_t(width) * 4);
  }
  Transition(p.texture, plume::RenderTextureLayout::COPY_DEST);
  FlushTransitions();
  lists_[slot_]->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(p.texture.texture.get()),
      plume::RenderTextureCopyLocation::PlacedFootprint(span.buffer, p.texture.format, width, height, 1, pitch / 4,
                                                        span.offset));
  Transition(p.texture, plume::RenderTextureLayout::SHADER_READ);
  p.ok = true;
}

// The native bob follows the finger direction; hit-testing stays at the input position.
// Size and motion follow the game image inside its letterbox.
void Backend::DrawPointer(plume::RenderTexture* target, plume::RenderFramebuffer* framebuffer, uint32_t width,
                          uint32_t height, HostResource* owner) {
  auto& p = pointer_;
  if (!p.ok || !p.texture.texture || g_pointer_suppressed.load(std::memory_order_relaxed)) return;
  // The internal frame is the whole game image; the swap chain image carries the letterbox around it.
  plume::RenderRect fit(0, 0, int32_t(width), int32_t(height));
  if (!owner) FitRect(width, height, uint32_t(cc2::display::AspectRatio() * 9000.0f + 0.5f), 9000, fit);
  const float fx = float(fit.left) + g_pointer_x.load(std::memory_order_relaxed) * float(fit.right - fit.left);
  const float fy = float(fit.top) + g_pointer_y.load(std::memory_order_relaxed) * float(fit.bottom - fit.top);
  const float edge = float(fit.bottom - fit.top) * float(std::clamp(REXCVAR_GET(gpu_pointer_size), 4.0, 60.0)) / 100.0f;
  const float scale = edge / float(p.sprite.height);
  const auto idle_start = std::chrono::nanoseconds(g_pointer_idle_start_ns.load(std::memory_order_acquire));
  const double seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now().time_since_epoch() - idle_start).count() - 1.0;
  const float bob = PointerBobOffset(p.sprite, seconds) * scale;
  const int32_t left = int32_t(fx - float(p.sprite.hotspot_x) * scale + bob * p.sprite.bob_direction_x);
  const int32_t top = int32_t(fy - float(p.sprite.hotspot_y) * scale + bob * p.sprite.bob_direction_y);
  const plume::RenderRect dest(left, top, left + int32_t(edge), top + int32_t(edge));
  Blit(p.texture, plume::RenderRect(0, 0, int32_t(p.sprite.width), int32_t(p.sprite.height)), target, framebuffer,
       owner ? owner->format : kSwapChainFormat, dest, 1.0f, true, true, owner);
}

// ------------------------------------------------------------------------------------------------ draws

UploadSpan Backend::Upload(uint64_t size, uint64_t align) {
  auto& chunks = uploads_[slot_];
  for (auto& chunk : chunks) {
    const uint64_t at = (chunk.used + align - 1) & ~(align - 1);
    if (at + size <= chunk.size) {
      chunk.used = at + size;
      return UploadSpan{chunk.buffer.get(), at, chunk.mapped + at};
    }
  }
  // Overflow grows the slot instead of dropping data.
  UploadChunk chunk;
  chunk.size = std::max(kUploadChunkSize, (size + align + 0xFFFF) & ~0xFFFFull);
  const plume::RenderBufferFlags flags = plume::RenderBufferFlag::CONSTANT | plume::RenderBufferFlag::INDEX |
                                         plume::RenderBufferFlag::STORAGE;
  chunk.buffer = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(chunk.size, flags));
  if (!chunk.buffer) return {};
  chunk.mapped = static_cast<uint8_t*>(chunk.buffer->map());
  if (!chunk.mapped) return {};
  chunk.used = size;
  chunks.push_back(std::move(chunk));
  auto& added = chunks.back();
  return UploadSpan{added.buffer.get(), 0, added.mapped};
}

uint32_t Backend::AllocateDescriptor() {
  if (!recycled_descriptors_.empty()) {
    const uint32_t index = recycled_descriptors_.back();
    recycled_descriptors_.pop_back();
    return index;
  }
  // Skip the reserved blanks under the directory-pack clamp and the container clamp target.
  while (next_descriptor_ >= kBlank3D && next_descriptor_ <= 2047) ++next_descriptor_;
  if (next_descriptor_ >= kTextureHeap - 1) return kBlank2D;
  return next_descriptor_++;
}

uint32_t Backend::DescriptorFor(HostResource& resource, uint8_t dimension, const std::array<uint8_t, 4>& swizzle) {
  const uint32_t key = uint32_t(dimension) | (uint32_t(swizzle[0]) << 8) | (uint32_t(swizzle[1]) << 12) |
                       (uint32_t(swizzle[2]) << 16) | (uint32_t(swizzle[3]) << 20);
  auto it = resource.views.find(key);
  if (it != resource.views.end()) return it->second;
  // Binding dimensions: 1 = 2D array view (a cube shows face 0, a stack all its layers), 2 = 3D, 3 = cube; a shape
  // the view cannot back reads the matching blank.
  plume::RenderTextureViewDesc view;
  view.format = resource.format;
  if (dimension == 3 && resource.cube()) {
    view.dimension = plume::RenderTextureViewDimension::TEXTURE_CUBE;
  } else if (dimension == 2 && resource.volume()) {
    view = plume::RenderTextureViewDesc::Texture3D(resource.format);
  } else if (dimension == 1 && !resource.volume()) {
    view = plume::RenderTextureViewDesc::Texture2DArray(resource.format, resource.cube() ? 1 : resource.layers());
  } else {
    // A view type the texture cannot back reads the dimension-matched blank.
    return resource.views[key] = dimension == 3 ? kBlankCube : dimension == 2 ? kBlank3D : kBlank2D;
  }
  view.componentMapping = plume::RenderComponentMapping(ToPlumeSwizzle(swizzle[0]), ToPlumeSwizzle(swizzle[1]),
                                                        ToPlumeSwizzle(swizzle[2]), ToPlumeSwizzle(swizzle[3]));
  auto object = resource.texture->createTextureView(view);
  const uint32_t index = object ? AllocateDescriptor() : kBlank2D;
  if (object && index != kBlank2D) {
    texture_set_->setTexture(index, resource.texture.get(), plume::RenderTextureLayout::SHADER_READ, object.get());
    resource.view_objects.push_back(std::move(object));
  }
  return resource.views[key] = index;
}

// Every distinct sampler state owns a slot in one shader-visible table, so a draw's bindings resolve to exact
// per-axis address modes. The table is only as large as the pack's shader clamp; when it is full, a slot no frame in
// flight still reads is rewritten, else the draw borrows the closest state, address modes first.
plume::RenderDescriptorSet* Backend::SamplerSet(const DrawRecord& r, std::array<uint32_t, 32> (&indices)[2]) {
  auto write = [&](uint32_t slot, uint32_t state) {
    auto& object = sampler_objects_[state];
    if (!object) object = device_->createSampler(SamplerDescFor(state));
    if (!object) return false;
    sampler_set_->setSampler(slot, object.get());
    sampler_slots_[state] = slot;
    sampler_slot_state_[slot] = state;
    return true;
  };
  for (const auto& s : r.samplers) {
    if (s.stage >= 2 || s.slot >= 32) continue;
    uint32_t slot = 0;
    bool found = false;
    if (auto it = sampler_slots_.find(s.state); it != sampler_slots_.end()) {
      slot = it->second;
      found = true;
    } else if (sampler_slot_count_ < sampler_capacity_) {
      if (write(sampler_slot_count_, s.state)) {
        slot = sampler_slot_count_++;
        found = true;
      }
    } else {
      // Least recently used slot whose last frame has retired; the current and in-flight frames still read theirs.
      uint32_t victim = UINT32_MAX;
      for (uint32_t i = 0; i < sampler_slot_count_; ++i) {
        if (sampler_slot_frame_[i] + kFramesInFlight > frame_counter_) continue;
        if (victim == UINT32_MAX || sampler_slot_frame_[i] < sampler_slot_frame_[victim]) victim = i;
      }
      if (victim != UINT32_MAX) {
        sampler_slots_.erase(sampler_slot_state_[victim]);
        if (write(victim, s.state)) {
          slot = victim;
          found = true;
        }
      }
    }
    if (!found) {
      // Closest live state: same address modes outweigh same filters, then border and anisotropy; LOD clamps last.
      int best = -1;
      for (uint32_t i = 0; i < sampler_slot_count_; ++i) {
        const uint32_t other = sampler_slot_state_[i];
        if (!sampler_slots_.count(other)) continue;
        const uint32_t diff = other ^ s.state;
        const int score = ((diff & 0x1FFu) ? 0 : 8) + ((diff & 0x1E00u) ? 0 : 4) + ((diff & 0x3E000u) ? 0 : 2) +
                          ((diff & 0x3FC0000u) ? 0 : 1);
        if (score > best) best = score, slot = i;
      }
    }
    sampler_slot_frame_[slot] = frame_counter_;
    indices[s.stage][s.slot] = slot;
  }
  return sampler_set_.get();
}

// Places one stream (or the index run) in shared memory: unregistered ranges once per frame, registered ranges
// once per version, key 0 privately for the draw; false when the bytes are missing or memory is full.
bool Backend::Place(StreamData& stream, uint64_t& offset, std::vector<std::pair<uint64_t, UploadSpan>>& copies,
                    std::vector<uint64_t>& copy_sizes) {
  if (stream.key && !stream.persistent) {
    // Unregistered ranges are uploaded once per frame; later draws of the frame reuse that copy.
    auto it = frame_ranges_.find(stream.key);
    if (it != frame_ranges_.end() && !stream.bytes) {
      offset = it->second;
    } else if (!stream.bytes) {
      return false;
    } else {
      if (!AllocateTransient(stream.size, offset)) {
        REXGPU_ERROR("[gpu] transient shared memory is full ({:#x} bytes wanted); draw dropped", stream.size);
        return false;
      }
      UploadSpan span = Upload(stream.size, 16);
      if (!span.data) {
        return false;
      }
      std::memcpy(span.data, stream.bytes->data(), stream.size);
      copies.emplace_back(offset, span);
      copy_sizes.push_back(stream.size);
      frame_ranges_[stream.key] = offset;
    }
  } else if (stream.key) {
    auto& range = persistent_ranges_[stream.key];
    const bool resident = range.size && range.version == stream.version;
    range.last_frame = frame_counter_;
    if (!resident) {
      if (!stream.bytes) {
        // The guest believed this version was resident; telling it otherwise makes the next frame resend the bytes.
        NoteEvicted(stream.key);
        return false;
      }
      if (range.size) persistent_.Free(range.offset, range.size, frame_counter_);
      range = {};
      range.last_frame = frame_counter_;
      if (!persistent_.Allocate(stream.size, range.offset) && !EvictPersistent(stream.size, range.offset)) {
        REXGPU_ERROR("[gpu] persistent shared memory is full ({:#x} bytes wanted)", stream.size);
        return false;
      }
      range.size = stream.size;
      range.version = stream.version;
      UploadSpan span = Upload(stream.size, 16);
      if (!span.data) {
        return false;
      }
      std::memcpy(span.data, stream.bytes->data(), stream.size);
      copies.emplace_back(range.offset, span);
      copy_sizes.push_back(stream.size);
    }
    offset = range.offset;
  } else {
    if (!stream.bytes || !AllocateTransient(stream.size, offset)) {
      if (stream.bytes) {
        REXGPU_ERROR("[gpu] transient shared memory is full ({:#x} bytes wanted); draw dropped", stream.size);
      }
      return false;
    }
    UploadSpan span = Upload(stream.size, 16);
    if (!span.data) {
      return false;
    }
    std::memcpy(span.data, stream.bytes->data(), stream.size);
    copies.emplace_back(offset, span);
    copy_sizes.push_back(stream.size);
  }
  return true;
}

// Frees owned ranges idle for a while, least recently drawn first, until the allocation fits. Levels leave their
// geometry registered after unloading, so this is what bounds the persistent area. The guest learns each dropped
// key through TakeEvicted; the idle threshold keeps a frame it already captured without bytes from losing its range.
bool Backend::EvictPersistent(uint64_t size, uint64_t& offset) {
  constexpr uint64_t kIdleFrames = 16;
  if (frame_counter_ <= kFramesInFlight) return false;
  const uint64_t retired = frame_counter_ - kFramesInFlight - 1;
  std::vector<std::pair<uint64_t, uint64_t>> idle;  // last frame, key
  for (const auto& [key, range] : persistent_ranges_) {
    if (range.size && range.last_frame + kIdleFrames <= frame_counter_) idle.emplace_back(range.last_frame, key);
  }
  std::sort(idle.begin(), idle.end());
  uint64_t freed = 0;
  for (const auto& [frame, key] : idle) {
    auto it = persistent_ranges_.find(key);
    persistent_.Free(it->second.offset, it->second.size, frame);
    freed += it->second.size;
    persistent_ranges_.erase(it);
    NoteEvicted(key);
    // Retire hands the freed ranges back at once because their frames are already complete.
    persistent_.Retire(retired);
    if (freed >= size && persistent_.Allocate(size, offset)) return true;
  }
  persistent_.Retire(retired);
  return persistent_.Allocate(size, offset);
}

// Places every upload a packet carries before any of its records run, so no upload splits a render pass: vertex
// ranges into shared memory, texture contents into their host textures.
void Backend::PrepareUploads(Packet& packet) {
  std::vector<std::pair<uint64_t, UploadSpan>> copies;  // shared-memory offset, staged bytes
  std::vector<uint64_t> copy_sizes;
  bool any = false;
  frame_ranges_.clear();
  for (auto& record : packet.records) {
    auto* draw = std::get_if<DrawRecord>(&record);
    if (!draw || !draw->constants) continue;
    if (!any) {
      BeginList();
      any = true;
    }
    auto& fetch = draw->constants->fetch;
    bool ok = true;
    for (auto& stream : draw->streams) {
      uint64_t offset = 0;
      if (!Place(stream, offset, copies, copy_sizes)) {
        ok = false;
        break;
      }
      const uint32_t at = (stream.fetch_index / 3) * 6 + (stream.fetch_index % 3) * 2;
      fetch[at] = uint32_t(offset) | (fetch[at] & 3);
    }
    if (ok && draw->indexed && !Place(draw->index, draw->index_offset, copies, copy_sizes)) ok = false;
    if (!ok) {
      draw->count = 0;
      continue;
    }
    for (auto& binding : draw->textures) {
      if (!binding.upload) continue;
      HostResource* texture = Get(binding.texture);
      if (!texture || !texture->sampled_only() || texture->version == binding.version) continue;
      binding.upload->Wait();
      Transition(*texture, plume::RenderTextureLayout::COPY_DEST);
      FlushTransitions();
      const uint32_t block = HostBlockSize(binding.texture.format);
      const uint32_t bpb = HostBlockBytes(binding.texture.format);
      const uint32_t levels = texture->mips();
      const uint32_t layers = texture->layers();
      auto* list = lists_[slot_].get();
      for (uint32_t layer = 0; layer < layers; ++layer) {
        for (uint32_t level = 0; level < levels; ++level) {
          const size_t i = size_t(layer) * levels + level;
          if (i >= binding.upload->levels.size()) break;
          const auto& bytes = binding.upload->levels[i];
          const uint32_t src_pitch = binding.upload->row_pitch[i];
          const uint32_t lw = std::max(binding.texture.width >> level, 1u);
          const uint32_t lh = std::max(binding.texture.height >> level, 1u);
          const uint32_t ld = texture->volume() ? std::max(binding.texture.depth >> level, 1u) : 1u;
          // A 3D level holds its slices back to back; the footprint takes the rows of one slice.
          const uint32_t rows = src_pitch ? uint32_t(bytes.size() / src_pitch) : 0;
          const uint32_t pitch = (src_pitch + 255) & ~255u;
          UploadSpan span = Upload(uint64_t(pitch) * rows, 512);
          if (!span.data || !rows) continue;
          for (uint32_t y = 0; y < rows; ++y) std::memcpy(span.data + size_t(y) * pitch, &bytes[size_t(y) * src_pitch], src_pitch);
          list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture->texture.get(), level, layer),
                                  plume::RenderTextureCopyLocation::PlacedFootprint(
                                      span.buffer, texture->format, lw, lh, ld, pitch / bpb * block, span.offset));
        }
      }
      texture->version = binding.version;
    }
  }
  if (copies.empty()) return;
  auto* list = lists_[slot_].get();
  Unbind();
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderBufferBarrier(shared_memory_.get(), plume::RenderBufferAccess::WRITE));
  for (size_t i = 0; i < copies.size(); ++i) {
    list->copyBufferRegion(shared_memory_->at(copies[i].first), copies[i].second.buffer->at(copies[i].second.offset),
                           copy_sizes[i]);
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE,
                 plume::RenderBufferBarrier(shared_memory_.get(), plume::RenderBufferAccess::READ));
}

plume::RenderShader* Backend::Shader(const ShaderEntry* entry, bool trimmed) {
  if (!entry) return nullptr;
  const uint64_t key = (uint64_t(entry->id) << 2) | (uint64_t(entry->stage) << 40) | (trimmed ? 1 : 0);
  auto& shader = shaders_[key];
  if (!shader) {
    const auto& bytes = trimmed ? entry->dxil_trim : entry->dxil;
    if (bytes.empty()) return nullptr;
    shader = device_->createShader(bytes.data(), bytes.size(), "main", plume::RenderShaderFormat::DXIL);
    if (!shader) REXGPU_ERROR("[gpu] cannot create shader {:016X}", entry->runtime_hash);
  }
  return shader.get();
}

// Worker for gpu_async_pipelines: driver compiles of new pipelines run here, off the render thread.
void Backend::CompileLoop() {
  for (;;) {
    std::pair<PipelineSlot*, plume::RenderGraphicsPipelineDesc> job;
    {
      std::unique_lock lock(compile_mutex_);
      compile_cv_.wait(lock, [this] { return compile_stop_ || !compile_queue_.empty(); });
      if (compile_stop_) return;
      job = compile_queue_.front();
      compile_queue_.pop_front();
    }
    job.first->pipeline = device_->createGraphicsPipeline(job.second);
    if (!job.first->pipeline) REXGPU_ERROR("[gpu] cannot create a draw pipeline");
    job.first->ready.store(true, std::memory_order_release);
    job.first->ready.notify_all();
    if (job.first->warm) warm_pending_.fetch_sub(1, std::memory_order_acq_rel);
  }
}

plume::RenderPipeline* Backend::DrawPipeline(const DrawRecord& r, uint32_t samples, bool wait) {
  PipelineState s = r.state;
  s.slope_bias *= float(ScaleFor(r.state.color_count ? r.color[0] : r.depth)) / 100.0f;
  const uint64_t key = XXH3_64bits(&s, sizeof(s)) ^ (r.depth_clip ? 0x5A5A : 0) ^ (uint64_t(samples) << 48);
  auto& slot = draw_pipelines_[key];
  if (slot) {
    if (slot->ready.load(std::memory_order_acquire)) return slot->pipeline.get();
    if (!wait) return nullptr;
    slot->ready.wait(false, std::memory_order_acquire);
    return slot->pipeline.get();
  }
  slot = std::make_unique<PipelineSlot>();
  slot->warm = warming_;
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = draw_layout_.get();
  desc.vertexShader = Shader(r.vs, s.use_trimmed_vs != 0);
  desc.pixelShader = Shader(r.ps, false);
  if (!desc.vertexShader || (r.ps && !desc.pixelShader)) {
    slot->ready.store(true, std::memory_order_release);
    return nullptr;
  }
  const auto geometry = GeometryExpansion(s.geometry);
  if (geometry != GeometryExpansion::kNone) {
    desc.geometryShader = Shader(BuiltinShader(geometry == GeometryExpansion::kRectList ? "rect_expand" : "point_expand"), false);
    if (!desc.geometryShader) { slot->ready.store(true, std::memory_order_release); return nullptr; }
  } else if (s.clip_planes) {
    desc.geometryShader = Shader(BuiltinShader("clip_planes"), false);
    if (!desc.geometryShader) { slot->ready.store(true, std::memory_order_release); return nullptr; }
  }
  desc.multisampling.sampleCount = plume::RenderSampleCounts(samples);
  // Slopes are per host pixel, so a scaled target scales them too, as the SDK's pipeline cache does.
  desc.depthBias = s.depth_bias;
  desc.slopeScaledDepthBias = s.slope_bias;
  switch (xenos::PrimitiveType(s.topology)) {
    case xenos::PrimitiveType::kPointList: desc.primitiveTopology = plume::RenderPrimitiveTopology::POINT_LIST; break;
    case xenos::PrimitiveType::kLineList: desc.primitiveTopology = plume::RenderPrimitiveTopology::LINE_LIST; break;
    case xenos::PrimitiveType::kLineStrip: desc.primitiveTopology = plume::RenderPrimitiveTopology::LINE_STRIP; break;
    case xenos::PrimitiveType::kTriangleStrip: desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_STRIP; break;
    default: desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST; break;
  }
  // Rectangles are never culled on Xenos.
  desc.cullMode = geometry == GeometryExpansion::kRectList ? plume::RenderCullMode::NONE
                  : s.cull == 1                            ? plume::RenderCullMode::FRONT
                  : s.cull == 2                            ? plume::RenderCullMode::BACK
                                                           : plume::RenderCullMode::NONE;
  desc.frontFace = s.front_ccw ? plume::RenderFrontFace::COUNTER_CLOCKWISE : plume::RenderFrontFace::CLOCKWISE;
  desc.depthClipEnabled = r.depth_clip;
  if (s.depth_format) {
    desc.depthTargetFormat = ToPlume(HostFormat(s.depth_format));
    desc.depthEnabled = s.depth_enable;
    desc.depthWriteEnabled = s.depth_enable && s.depth_write;
    desc.depthFunction = ToPlumeCompare(s.depth_func);
    desc.stencilEnabled = s.stencil_enable;
    desc.stencilReadMask = s.stencil_read_mask;
    desc.stencilWriteMask = s.stencil_write_mask;
    desc.stencilReference = s.stencil_ref;
    desc.stencilFrontFace = {ToPlumeStencil(s.stencil_pass), ToPlumeStencil(s.stencil_fail),
                             ToPlumeStencil(s.stencil_zfail), ToPlumeCompare(s.stencil_func)};
    desc.stencilBackFace = {ToPlumeStencil(s.stencil_pass_bf), ToPlumeStencil(s.stencil_fail_bf),
                            ToPlumeStencil(s.stencil_zfail_bf), ToPlumeCompare(s.stencil_func_bf)};
  }
  desc.renderTargetCount = s.color_count;
  for (uint32_t i = 0; i < s.color_count; ++i) {
    desc.renderTargetFormat[i] = ToPlume(HostFormat(s.color_format[i]));
    reg::RB_BLENDCONTROL b;
    b.value = s.blend[i];
    auto& blend = desc.renderTargetBlend[i];
    blend.blendEnabled = s.blend_enable[i] != 0;
    blend.srcBlend = ToPlumeBlend(b.color_srcblend);
    blend.dstBlend = ToPlumeBlend(b.color_destblend);
    blend.blendOp = ToPlumeBlendOp(b.color_comb_fcn);
    blend.srcBlendAlpha = ToPlumeAlphaBlend(b.alpha_srcblend);
    blend.dstBlendAlpha = ToPlumeAlphaBlend(b.alpha_destblend);
    blend.blendOpAlpha = ToPlumeBlendOp(b.alpha_comb_fcn);
    blend.renderTargetWriteMask = s.write_mask[i];
  }
  if (!warming_) RecordPipeline(r, samples);
  if (REXCVAR_GET(gpu_async_pipelines)) {
    if (compile_threads_.empty()) {
      for (unsigned i = 0; i < 2; ++i) compile_threads_.emplace_back([this] { CompileLoop(); });
    }
    if (slot->warm) warm_pending_.fetch_add(1, std::memory_order_acq_rel);
    {
      std::lock_guard lock(compile_mutex_);
      compile_queue_.emplace_back(slot.get(), desc);
    }
    compile_cv_.notify_one();
    if (!wait) return nullptr;
    slot->ready.wait(false, std::memory_order_acquire);
    return slot->pipeline.get();
  }
  slot->pipeline = device_->createGraphicsPipeline(desc);
  slot->ready.store(true, std::memory_order_release);
  if (!slot->pipeline) {
    REXGPU_ERROR("[gpu] cannot create a draw pipeline (vs {:016X} ps {:016X})", r.vs->runtime_hash,
                 r.ps ? r.ps->runtime_hash : 0);
  }
  return slot->pipeline.get();
}

plume::RenderFramebuffer* Backend::DrawFramebuffer(const DrawRecord& r) {
  std::array<uint32_t, 5> key{};
  for (uint32_t i = 0; i < r.state.color_count; ++i) key[i] = r.color[i].id;
  key[4] = r.state.depth_format ? r.depth.id : 0;
  auto& framebuffer = draw_framebuffers_[key];
  if (!framebuffer) {
    const plume::RenderTexture* attachments[4] = {};
    uint32_t samples = 0;
    bool mismatch = false;
    auto check = [&](const HostResource& t) {
      mismatch |= samples && t.samples != samples;
      samples = t.samples;
    };
    for (uint32_t i = 0; i < r.state.color_count; ++i) {
      HostResource* t = Get(r.color[i]);
      if (!t) return nullptr;
      attachments[i] = t->texture.get();
      check(*t);
    }
    const plume::RenderTexture* depth = nullptr;
    if (key[4]) {
      HostResource* d = Get(r.depth);
      if (!d) return nullptr;
      depth = d->texture.get();
      check(*d);
    }
    // The guest can pair surfaces of different sample counts; the host framebuffer cannot.
    if (mismatch) {
      static std::atomic<bool> once{false};
      if (!once.exchange(true)) REXGPU_ERROR("[gpu] a draw binds targets of different sample counts; dropped");
      draw_framebuffers_.erase(key);
      return nullptr;
    }
    framebuffer = device_->createFramebuffer(plume::RenderFramebufferDesc(attachments, r.state.color_count, depth));
  }
  return framebuffer.get();
}

void Backend::Execute(DrawRecord& r) {
  if (!draws_ready_ || !r.constants || !r.count) return;
  BeginList();
  auto* list = lists_[slot_].get();
  const PackAbi& abi = ShaderPackAbi();

  // Descriptor indices per stage, with signed twins and missing textures on the matching blank.
  std::array<uint32_t, 32> indices[2] = {};
  const ShaderEntry* entries[2] = {r.vs, r.ps};
  for (uint32_t stage = 0; stage < 2; ++stage) {
    if (!entries[stage]) continue;
    const auto& bindings = entries[stage]->bindings;
    for (uint32_t slot = 0; slot < bindings.size() && slot < 32; ++slot) {
      const uint8_t dim = bindings[slot].dimension;
      indices[stage][slot] = bindings[slot].sampler ? 0 : dim == 3 ? kBlankCube : dim == 2 ? kBlank3D : kBlank2D;
    }
  }
  for (const auto& binding : r.textures) {
    HostResource* texture = Get(binding.texture);
    uint32_t index = binding.dimension == 3 ? kBlankCube : binding.dimension == 2 ? kBlank3D : kBlank2D;
    if (texture && binding.texture.format != HostFormat::kUnsupported) {
      if (!(texture->sampled_only() && texture->version == 0)) {
        Transition(*texture, plume::RenderTextureLayout::SHADER_READ);
        index = DescriptorFor(*texture, binding.dimension, binding.swizzle);
      }
    } else if (texture) {
      index = kBlank2D;
    }
    if (binding.stage < 2 && binding.slot < 32) {
      indices[binding.stage][binding.slot] = index;
      // The signed twin (t:N:D:1) follows its unsigned slot and gets the same view.
      const ShaderEntry* e = entries[binding.stage];
      if (e && binding.slot + 1u < e->bindings.size() && e->bindings[binding.slot + 1].is_signed &&
          !e->bindings[binding.slot + 1].sampler) {
        indices[binding.stage][binding.slot + 1] = index;
      }
    }
  }
  plume::RenderDescriptorSet* samplers = SamplerSet(r, indices);

  // Every attachment of a framebuffer has the same sample count; the size source sets the viewport scale.
  HostResource* size_source = r.state.color_count ? Get(r.color[0]) : Get(r.depth);
  if (!size_source) return;
  const uint32_t samples = size_source->samples;
  plume::RenderPipeline* pipeline = DrawPipeline(r, samples);
  plume::RenderFramebuffer* framebuffer = pipeline ? DrawFramebuffer(r) : nullptr;
  if (!pipeline || !framebuffer) return;
  for (uint32_t i = 0; i < r.state.color_count; ++i) {
    if (HostResource* t = Get(r.color[i])) Transition(*t, plume::RenderTextureLayout::COLOR_WRITE);
  }
  if (r.state.depth_format) {
    if (HostResource* d = Get(r.depth)) Transition(*d, plume::RenderTextureLayout::DEPTH_WRITE);
  }
  FlushTransitions();

  // Constant buffers, 256-byte aligned for root CBVs.
  const DrawConstants& c = *r.constants;
  auto put = [&](const void* data, uint64_t size, uint64_t reserve) {
    UploadSpan span = Upload(std::max(size, reserve), 256);
    if (span.data) {
      std::memcpy(span.data, data, size);
      if (reserve > size) std::memset(span.data + size, 0, reserve - size);
    }
    return span;
  };
  SystemConstants system = c.system;
  system.pixel_position_scale[0] = float(size_source->desc.width) / float(size_source->width);
  system.pixel_position_scale[1] = float(size_source->desc.height) / float(size_source->height);
  const UploadSpan b0 = put(&system, abi.system_size, 512);
  auto put_float = [&](const auto& values) {
    const uint64_t key = XXH3_64bits(values.data(), sizeof(values));
    if (REXCVAR_GET(gpu_constant_reuse)) {
      const auto [first, last] = constant_uploads_.equal_range(key);
      for (auto it = first; it != last; ++it) {
        if (std::memcmp(it->second.source, values.data(), sizeof(values)) == 0) return it->second.span;
      }
    }
    UploadSpan span = put(values.data(), sizeof(values), 0);
    if (span.data && REXCVAR_GET(gpu_constant_reuse)) constant_uploads_.emplace(key, ConstantUpload{values.data(), span});
    return span;
  };
  const UploadSpan b1_vs = put_float(c.vs_float);
  const UploadSpan b1_ps = put_float(c.ps_float);
  const UploadSpan b2 = put(c.bool_loop.data(), sizeof(c.bool_loop), 256);
  const UploadSpan b3 = put(c.fetch.data(), sizeof(c.fetch), 768);
  const UploadSpan b4_vs = put(indices[0].data(), sizeof(indices[0]), 256);
  const UploadSpan b4_ps = put(indices[1].data(), sizeof(indices[1]), 256);
  const UploadSpan b5 = put(c.vfetch.data(), sizeof(c.vfetch), 512);
  if (!b0.data || !b1_vs.data || !b1_ps.data || !b2.data || !b3.data || !b4_vs.data || !b4_ps.data || !b5.data) return;

  if (bound_ != framebuffer) {
    Unbind();
    list->setFramebuffer(framebuffer);
    bound_ = framebuffer;
  }
  list->setGraphicsPipelineLayout(draw_layout_.get());
  list->setPipeline(pipeline);
  list->setGraphicsDescriptorSet(samplers, 0);
  list->setGraphicsDescriptorSet(texture_set_.get(), 1);
  list->setGraphicsDescriptorSet(texture_set_.get(), 2);
  list->setGraphicsDescriptorSet(texture_set_.get(), 3);
  list->setGraphicsRootDescriptor(b0.buffer->at(b0.offset), kRootSystem);
  list->setGraphicsRootDescriptor(b1_vs.buffer->at(b1_vs.offset), kRootFloatsVs);
  list->setGraphicsRootDescriptor(b1_ps.buffer->at(b1_ps.offset), kRootFloatsPs);
  list->setGraphicsRootDescriptor(b2.buffer->at(b2.offset), kRootBoolLoop);
  list->setGraphicsRootDescriptor(b3.buffer->at(b3.offset), kRootFetch);
  list->setGraphicsRootDescriptor(b4_vs.buffer->at(b4_vs.offset), kRootIndicesVs);
  list->setGraphicsRootDescriptor(b4_ps.buffer->at(b4_ps.offset), kRootIndicesPs);
  list->setGraphicsRootDescriptor(shared_memory_->at(0), kRootShared);
  list->setGraphicsRootDescriptor(dummy_uav_->at(0), kRootSharedUav);
  if (abi.vfetch_table) list->setGraphicsRootDescriptor(b5.buffer->at(b5.offset), kRootVfetch);
  // Guest pixels to host texels: the viewport by the target's exact ratio, the scissor edges as resolves round them.
  const float fx = float(size_source->width) / float(size_source->desc.width);
  const float fy = float(size_source->height) / float(size_source->desc.height);
  list->setViewports(plume::RenderViewport(r.viewport[0] * fx, r.viewport[1] * fy, r.viewport[2] * fx,
                                           r.viewport[3] * fy, r.viewport[4], r.viewport[5]));
  list->setScissors(plume::RenderRect(std::min(Scaled(*size_source, r.scissor.left), int32_t(size_source->width)),
                                      std::min(Scaled(*size_source, r.scissor.top), int32_t(size_source->height)),
                                      std::min(Scaled(*size_source, r.scissor.right), int32_t(size_source->width)),
                                      std::min(Scaled(*size_source, r.scissor.bottom), int32_t(size_source->height))));
  if (r.blend_constant_used) list->setBlendFactor(r.blend_constant);
  if (r.indexed) {
    const plume::RenderIndexBufferView view(shared_memory_->at(r.index_offset), r.index.size,
                                            r.index32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
    list->setIndexBuffer(&view);
    list->drawIndexedInstanced(r.count, 1, 0, 0, 0);
  } else {
    list->drawInstanced(r.count, 1, 0, 0);
  }
}

plume::RenderPipeline* Backend::ResizePipeline(const HostResource& source, uint32_t stencil_mask) {
  const uint32_t variant = (source.depth() ? (stencil_mask ? 4u : 2u) : 0u) + (source.samples > 1 ? 1u : 0u);
  auto& shader = resize_shaders_[variant];
  if (!shader) {
    struct Blob { const uint8_t* data; size_t size; const char* entry; };
    const Blob blobs[] = {
        {shaders::kResize_color_dxil, sizeof(shaders::kResize_color_dxil), "ColorMain"},
        {shaders::kResize_color_ms_dxil, sizeof(shaders::kResize_color_ms_dxil), "ColorMsMain"},
        {shaders::kResize_depth_dxil, sizeof(shaders::kResize_depth_dxil), "DepthMain"},
        {shaders::kResize_depth_ms_dxil, sizeof(shaders::kResize_depth_ms_dxil), "DepthMsMain"},
        {shaders::kResize_stencil_dxil, sizeof(shaders::kResize_stencil_dxil), "StencilMain"},
        {shaders::kResize_stencil_ms_dxil, sizeof(shaders::kResize_stencil_ms_dxil), "StencilMsMain"}};
    const auto& blob = blobs[variant];
    shader = device_->createShader(blob.data, blob.size, blob.entry, plume::RenderShaderFormat::DXIL);
    if (!shader) return nullptr;
  }
  const uint64_t key = uint64_t(source.format) | (uint64_t(source.samples) << 16) |
                       (uint64_t(variant) << 24) | (uint64_t(stencil_mask) << 32);
  auto& pipeline = resize_pipelines_[key];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout_.get();
    desc.vertexShader = resolve_vs_.get();
    desc.pixelShader = shader.get();
    desc.multisampling.sampleCount = plume::RenderSampleCounts(source.samples);
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
    if (source.depth()) {
      desc.depthTargetFormat = source.format;
      desc.depthEnabled = !stencil_mask;
      desc.depthWriteEnabled = !stencil_mask;
      desc.stencilEnabled = stencil_mask != 0;
      desc.stencilWriteMask = uint8_t(stencil_mask);
      desc.stencilReference = stencil_mask;
      desc.stencilFrontFace = desc.stencilBackFace = {
          plume::RenderStencilOp::REPLACE, plume::RenderStencilOp::KEEP,
          plume::RenderStencilOp::KEEP, plume::RenderComparisonFunction::ALWAYS};
    } else {
      desc.renderTargetCount = 1;
      desc.renderTargetFormat[0] = source.format;
      desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    }
    pipeline = device_->createGraphicsPipeline(desc);
  }
  return pipeline.get();
}

// Scale changes run after Swap; only targets whose scale changed are replaced, keeping their colour, depth and stencil
// contents.
void Backend::ResizeTargets(uint32_t scale, uint32_t shadow_scale) {
  const uint64_t wanted = (uint64_t(scale) << 32) | shadow_scale;
  if ((scale == scale_pct_ && shadow_scale == shadow_scale_pct_) || wanted == failed_scales_) return;
  failed_scales_ = wanted;  // cleared once the new targets are in place
  auto rescaled = [&](const HostResource& resource) {
    return !resource.sampled_only() && resource.scale != (CourseShadow(resource.desc) ? shadow_scale : scale);
  };
  FlushTransitions();
  SubmitAndWait();
  WaitAll();
  for (auto& [id, source] : resources_) {
    if (!rescaled(source)) continue;
    if (!ResizePipeline(source, 0) || !SampleSet(source)) return;
    if (source.depth()) {
      for (uint32_t mask = 1; mask <= 128; mask <<= 1) if (!ResizePipeline(source, mask)) return;
    }
  }
  const uint32_t previous_scale = scale_pct_, previous_shadow_scale = shadow_scale_pct_;
  std::unordered_map<uint32_t, HostResource> previous;
  for (auto it = resources_.begin(); it != resources_.end();) {
    if (!rescaled(it->second)) { ++it; continue; }
    previous.insert(resources_.extract(it++));
  }
  draw_framebuffers_.clear();
  scale_pct_ = scale;
  shadow_scale_pct_ = shadow_scale;
  auto rollback = [&] {
    FlushTransitions();
    SubmitAndWait();
    for (const auto& [id, source] : previous) resources_.erase(id);
    resources_.merge(previous);
    scale_pct_ = previous_scale;
    shadow_scale_pct_ = previous_shadow_scale;
    REXGPU_ERROR("[display] could not resize render targets; keeping {}% (shadows {}%)", scale_pct_,
                 shadow_scale_pct_);
  };
  for (auto& [id, source] : previous) {
    if (!Get(source.desc)) { rollback(); return; }
  }
  std::vector<std::unique_ptr<plume::RenderDescriptorSet>> stencil_sets;
  for (auto& [id, source] : previous) {
    auto& dest = resources_.at(id);
    plume::RenderDescriptorSet* stencil_set = nullptr;
    if (source.depth()) {
      auto view_desc = plume::RenderTextureViewDesc::Texture2D(source.format);
      view_desc.stencilAspect = true;
      auto view = source.texture->createTextureView(view_desc);
      auto set = sample_set_builder_.create(device_.get());
      if (!view || !set) { rollback(); return; }
      set->setTexture(0, source.texture.get(), plume::RenderTextureLayout::SHADER_READ, view.get());
      source.view_objects.push_back(std::move(view));
      stencil_set = set.get();
      stencil_sets.push_back(std::move(set));
    }
    BeginList();
    Transition(source, plume::RenderTextureLayout::SHADER_READ);
    Transition(dest, dest.depth() ? plume::RenderTextureLayout::DEPTH_WRITE : plume::RenderTextureLayout::COLOR_WRITE);
    FlushTransitions();
    Unbind();
    auto* list = lists_[slot_].get();
    list->setFramebuffer(dest.framebuffer.get());
    bound_ = dest.framebuffer.get();
    list->setGraphicsPipelineLayout(layout_.get());
    list->setViewports(plume::RenderViewport(0, 0, float(dest.width), float(dest.height)));
    list->setScissors(plume::RenderRect(0, 0, int32_t(dest.width), int32_t(dest.height)));
    struct { float ratio[2]; uint32_t mask; uint32_t padding; } constants{
        {float(source.width) / dest.width, float(source.height) / dest.height}, 0, 0};
    auto copy = [&](uint32_t mask) {
      constants.mask = mask;
      list->setPipeline(ResizePipeline(source, mask));
      list->setGraphicsDescriptorSet(mask ? stencil_set : source.sample_set.get(), 0);
      list->setGraphicsPushConstants(0, &constants, 0, sizeof(constants));
      list->drawInstanced(3, 1, 0, 0);
    };
    copy(0);
    if (source.depth()) for (uint32_t mask = 1; mask <= 128; mask <<= 1) copy(mask);
  }
  SubmitAndWait();
  for (const auto& [id, source] : previous) {
    for (const auto& [key, index] : source.views) {
      if (index != kBlank2D && (index < kBlank3D || index > 2047)) recycled_descriptors_.push_back(index);
    }
  }
  failed_scales_ = 0;
  REXGPU_INFO("[display] internal resolution scale {}%, course shadows {}%; preserved {} render targets", scale_pct_,
              shadow_scale_pct_, previous.size());
}

// One record per draw pipeline: the state with the shader ids blanked, the shaders' pack hashes, and the sample
// count, behind a header naming the pack it was recorded against.
namespace {
struct PipelineRecordHeader {
  char magic[8] = {'C', 'C', '2', 'P', 'I', 'P', 'E', '1'};
  uint64_t identity = 0;
  uint32_t record_size = 0;
  uint32_t _pad = 0;
};
struct PipelineRecord {
  PipelineState state{};
  uint64_t vs_hash = 0;
  uint64_t ps_hash = 0;
  uint8_t depth_clip = 0;
  uint8_t samples = 1;
  uint8_t _pad[6] = {};
};
static_assert(sizeof(PipelineRecord) == 96);
}  // namespace

void Backend::RecordPipeline(const DrawRecord& r, uint32_t samples) {
  if (!REXCVAR_GET(gpu_pipeline_warmup) || !r.vs) return;
  // Specialised pixel variants carry per-process ids and no pack hash of their own.
  if (r.state.ps_entry >= 0x80000000u) return;
  const auto path = ShaderPackSidecar("pipelines.bin");
  if (path.empty()) return;
  PipelineRecord rec;
  rec.state = r.state;
  rec.state.vs_entry = 0;
  rec.state.ps_entry = 0;
  rec.vs_hash = r.vs->runtime_hash;
  rec.ps_hash = r.ps ? r.ps->runtime_hash : 0;
  rec.depth_clip = r.depth_clip ? 1 : 0;
  rec.samples = uint8_t(samples);
  const uint64_t key = XXH3_64bits(&rec, sizeof(rec));
  if (!recorded_pipelines_.insert(key).second) return;
  std::error_code ec;
  const bool fresh = !std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) < sizeof(PipelineRecordHeader);
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) return;
  if (fresh) {
    PipelineRecordHeader header;
    header.identity = ShaderPackIdentity();
    header.record_size = sizeof(PipelineRecord);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
  }
  out.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
}

// Queues every recorded pipeline on the compile workers before the first frame, so the driver's compile cost lands
// in the loading screens instead of mid-level. A list from another pack is discarded.
void Backend::WarmPipelines() {
  if (!REXCVAR_GET(gpu_pipeline_warmup) || !REXCVAR_GET(gpu_async_pipelines)) return;
  const auto path = ShaderPackSidecar("pipelines.bin");
  if (path.empty()) return;
  std::ifstream in(path, std::ios::binary);
  if (!in) return;
  PipelineRecordHeader header;
  if (!in.read(reinterpret_cast<char*>(&header), sizeof(header)) || std::memcmp(header.magic, "CC2PIPE1", 8) != 0 ||
      header.identity != ShaderPackIdentity() || header.record_size != sizeof(PipelineRecord)) {
    in.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    REXGPU_INFO("[gpu] pipeline list belongs to another pack; starting a new one");
    return;
  }
  warming_ = true;
  uint32_t queued = 0, skipped = 0;
  PipelineRecord rec;
  while (in.read(reinterpret_cast<char*>(&rec), sizeof(rec))) {
    recorded_pipelines_.insert(XXH3_64bits(&rec, sizeof(rec)));
    const ShaderEntry* vs = FindShader(ShaderStage::kVertex, rec.vs_hash, rec.vs_hash);
    const ShaderEntry* ps = rec.ps_hash ? FindShader(ShaderStage::kPixel, rec.ps_hash, rec.ps_hash) : nullptr;
    if (!vs || (rec.ps_hash && !ps)) {
      ++skipped;
      continue;
    }
    DrawRecord r;
    r.state = rec.state;
    r.state.vs_entry = vs->id;
    r.state.ps_entry = ps ? ps->id : 0;
    r.vs = vs;
    r.ps = ps;
    r.depth_clip = rec.depth_clip != 0;
    DrawPipeline(r, rec.samples, false);
    ++queued;
  }
  warming_ = false;
  REXGPU_DEBUG("[gpu] pipeline warm-up: {} queued, {} skipped", queued, skipped);
}

void Backend::Execute(Packet& packet) {
  constant_uploads_.clear();
  if (draws_ready_) {
    for (auto& record : packet.records) {
      auto* draw = std::get_if<DrawRecord>(&record);
      if (!draw || !draw->constants || !draw->count) continue;
      draw->ps = SpecializePixelShader(draw->ps, draw->constants->bool_loop[4]);
      draw->state.ps_entry = draw->ps ? draw->ps->id : 0;
      const auto& target = draw->state.color_count ? draw->color[0] : draw->depth;
      if (REXCVAR_GET(gpu_async_pipelines)) DrawPipeline(*draw, HostSamples(target), false);
    }
  }
  if (draws_ready_) PrepareUploads(packet);
  for (auto& record : packet.records) {
    Execute(record);
    if (std::holds_alternative<SwapRecord>(record)) constant_uploads_.clear();
  }
}

// Frame queue shared by the guest threads and the render thread.
struct Renderer {
  Backend backend;
  std::thread thread;
  std::mutex mutex;
  std::condition_variable host_cv;   // packets available or stopping
  std::condition_variable guest_cv;  // a queued frame was taken
  std::deque<Packet> packets;
  size_t queued_frames = 0;
  bool running = false;
  bool stopping = false;
  std::mutex build_mutex;
  Packet building;
};

Renderer* g_renderer = nullptr;
std::mutex g_lifetime_mutex;

bool EndsWithSwap(const Packet& packet) {
  return !packet.records.empty() && std::holds_alternative<SwapRecord>(packet.records.back());
}

void RenderThread(Renderer* r) {
  for (;;) {
    Packet packet;
    {
      std::unique_lock lock(r->mutex);
      r->host_cv.wait(lock, [r] { return r->stopping || !r->packets.empty(); });
      if (r->stopping) break;
      packet = std::move(r->packets.front());
      r->packets.pop_front();
    }
    r->backend.Execute(packet);
    if (EndsWithSwap(packet)) {
      std::lock_guard lock(r->mutex);
      --r->queued_frames;
      r->guest_cv.notify_all();
    }
  }
  std::lock_guard lock(r->mutex);
  r->packets.clear();
  r->queued_frames = 0;
  r->guest_cv.notify_all();
}

void Push(Renderer* r, Packet packet) {
  std::unique_lock lock(r->mutex);
  r->guest_cv.wait(lock, [r] { return r->stopping || r->queued_frames < kMaxQueuedFrames; });
  if (r->stopping) return;
  if (EndsWithSwap(packet)) ++r->queued_frames;
  r->packets.push_back(std::move(packet));
  r->host_cv.notify_one();
}

}  // namespace

bool Initialize(rex::ui::Window* window) {
  std::lock_guard lifetime(g_lifetime_mutex);
  if (g_renderer) return true;
  auto* r = new Renderer;
  if (!window || !r->backend.Create(window)) {
    r->backend.Destroy();
    delete r;
    REXGPU_ERROR("[gpu] native renderer disabled; the window stays black");
    return false;
  }
  r->running = true;
  r->thread = std::thread(RenderThread, r);
  g_renderer = r;
  return true;
}

void Shutdown() {
  std::lock_guard lifetime(g_lifetime_mutex);
  Renderer* r = g_renderer;
  if (!r || !r->running) return;
  {
    std::lock_guard lock(r->mutex);
    r->stopping = true;
    r->host_cv.notify_all();
    r->guest_cv.notify_all();
  }
  if (r->thread.joinable()) r->thread.join();
  r->backend.Destroy();
  r->running = false;
  // The Renderer object stays allocated: guest threads may still call Submit, which sees stopping and drops.
  REXGPU_INFO("[gpu] renderer shut down");
}

void RequestResize() {
  if (g_renderer) g_renderer->backend.RequestResize();
}

void Submit(Record record) {
  Renderer* r = g_renderer;
  if (!r) return;
  std::lock_guard lock(r->build_mutex);
  r->building.records.push_back(std::move(record));
}

void SetOverlayFontRoot(const std::filesystem::path& folder) {
  std::lock_guard lock(g_font_mutex);
  g_font_root = folder;
}

uint64_t FrameIndex() { return g_frame_index.load(std::memory_order_relaxed); }

// Blocks the first presented frame until the start-up warm-up has compiled, so a first run after a pack change
// spends its compile time on the boot screen and never mid-level. Later frames never wait here.
void WaitForWarmup() {
  Renderer* r = g_renderer;
  const int32_t limit = REXCVAR_GET(gpu_pipeline_warmup_wait);
  if (!r || limit <= 0) return;
  static bool waited = false;
  if (waited) return;
  waited = true;
  const uint32_t total = r->backend.PendingWarmups();
  if (total == 0) return;
  const auto started = std::chrono::steady_clock::now();
  while (r->backend.PendingWarmups() != 0) {
    if (std::chrono::steady_clock::now() - started > std::chrono::seconds(limit)) {
      REXGPU_WARN("[gpu] pipeline warm-up still has {} of {} compiling after {} s; continuing", r->backend.PendingWarmups(),
                  total, limit);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REXGPU_DEBUG("[gpu] pipeline warm-up: {} compiled in {:.1f} s before the first frame", total,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
}

bool AnyEvicted() { return g_evicted_any.load(std::memory_order_acquire); }

void TakeEvicted(std::vector<uint64_t>& keys) {
  std::lock_guard lock(g_evicted_mutex);
  keys.swap(g_evicted);
  g_evicted.clear();
  g_evicted_any.store(false, std::memory_order_release);
}

void SetVsync(bool enabled) { g_vsync_request.store(enabled ? 1 : 0, std::memory_order_release); }

void SuppressPointer(bool suppressed) { g_pointer_suppressed.store(suppressed, std::memory_order_relaxed); }

void SetPointer(float x, float y, bool visible) {
  // Begin the native bob from rest after one second idle; repeated stationary updates keep the timer.
  if (x != g_pointer_x.load(std::memory_order_relaxed) ||
      y != g_pointer_y.load(std::memory_order_relaxed) ||
      (visible && !g_pointer_visible.load(std::memory_order_acquire))) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    g_pointer_idle_start_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(),
                                 std::memory_order_release);
  }
  g_pointer_x.store(x, std::memory_order_relaxed);
  g_pointer_y.store(y, std::memory_order_relaxed);
  g_pointer_visible.store(visible, std::memory_order_release);
}

void EndFrame() {
  g_frame_index.fetch_add(1, std::memory_order_relaxed);
  Renderer* r = g_renderer;
  if (!r) return;
  Packet packet;
  {
    std::lock_guard lock(r->build_mutex);
    packet = std::move(r->building);
    r->building = {};
    r->building.records.reserve(packet.records.size());
  }
  draw::FinishPreparation(packet);
  Push(r, std::move(packet));
}

}  // namespace cc2::gpu
