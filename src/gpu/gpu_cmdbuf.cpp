// Command-buffer record and replay. A recorded entry keeps the recording device's whole struct, the vertex shader
// bytes as patched for that draw, and the union of the D3DTAG bits the runtime had pending at each flush since
// BeginCommandBuffer; at RunCommandBuffer those tagged registers, the shaders and the draw's own buffers come from
// the recording and every other register from the live device, which is what the runtime's register inheritance
// does for a buffer whose inherit mask is all ones.

#include "gpu/gpu_cmdbuf.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <memory>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "gpu/gpu.h"
#include "gpu/gpu_memory.h"
#include "gpu/guest/d3d_device.h"

namespace cc2::gpu::cmdbuf {

namespace {

using guest::D3DDevice;

constexpr size_t kDeviceBytes = sizeof(D3DDevice);

template <typename T>
const T* Guest(uint32_t address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<const T*>(address);
}

struct BlendSet {
  uint32_t run;
  uint32_t index;
  uint32_t blend;
};

struct Entry {
  bool clear = false;
  draw::DrawArgs draw;
  capture::ClearArgs clear_args;
  std::vector<uint8_t> device;   // the recording device when the entry was flushed
  draw::VsSnapshot vs;           // the vertex shader as patched for this draw
  uint64_t tags[5] = {};         // D3DTAG bits the recording wrote by then, MSB-first per mask
  std::vector<BlendSet> blends;  // GpuSetBlendControl calls so far, in order
};

struct Buffer {
  uint32_t device = 0;
  uint32_t flags = 0;
  std::shared_ptr<const std::vector<Entry>> entries;
};

struct RecordingState {
  uint32_t buffer = 0;
  uint32_t flags = 0;
  uint64_t begin_pending[5] = {};  // tags pending before Begin: inherited, not recorded
  uint64_t tags[5] = {};
  uint32_t run = 0;
  std::vector<BlendSet> blends;
  std::vector<Entry> entries;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, RecordingState> g_recording;  // by device
std::unordered_map<uint32_t, Buffer> g_buffers;            // by buffer address
std::atomic<bool> g_warn_inline{false};
std::atomic<bool> g_warn_unknown{false};

void ReadPending(uint32_t device, uint64_t (&out)[5]) {
  const auto* d = Guest<D3DDevice>(device);
  for (int i = 0; i < 5; ++i) out[i] = d->m_Pending.m_Mask[i];
}

inline bool Bit(const uint64_t (&tags)[5], int mask, int bit) { return (tags[mask] >> (63 - bit)) & 1; }

// Copies the register range one D3DTAG bit names from the recording into the merged struct.
void CopyTag(uint8_t* merged, const uint8_t* snap, int mask, int bit) {
  auto copy = [&](size_t offset, size_t size) { std::memcpy(merged + offset, snap + offset, size); };
  switch (mask) {
    case 0: copy(offsetof(D3DDevice, m_Constants.VertexShaderF) + 64 * bit, 64); break;
    case 1: copy(offsetof(D3DDevice, m_Constants.PixelShaderF) + 64 * bit, 64); break;
    case 2:
      if (bit >= 6 && bit <= 21) {
        copy(offsetof(D3DDevice, m_DestinationPacket) + 4 * (bit - 6), 4);
        // The surface objects behind the colour and depth info registers travel with them.
        if (bit >= 7 && bit <= 11 && bit != 8) {
          const int n = bit == 7 ? 0 : bit - 8;
          copy(offsetof(D3DDevice, render_targets) + 4 * n, 4);
        }
        if (bit == 8) copy(offsetof(D3DDevice, depth_stencil), 4);
      } else if (bit >= 22 && bit <= 42) {
        copy(offsetof(D3DDevice, m_ValuesPacket) + 4 * (bit - 22), 4);
      } else if (bit == 43) {
        copy(offsetof(D3DDevice, pixel_shader), 4);
      } else if (bit == 44) {
        copy(offsetof(D3DDevice, vertex_shader), 4);
      } else if (bit >= 47 && bit <= 51) {
        copy(offsetof(D3DDevice, m_ProgramPacket) + 4 * (bit - 47), 4);
      } else if (bit >= 52) {
        copy(offsetof(D3DDevice, m_ControlPacket) + 4 * (bit - 52), 4);
      }
      break;
    case 3:
      if (bit >= 9 && bit <= 29) copy(offsetof(D3DDevice, m_TessellatorPacket) + 4 * (bit - 9), 4);
      else if (bit >= 32) copy(offsetof(D3DDevice, m_Constants.Fetch) + sizeof(guest::FetchConstant) * (bit - 32), sizeof(guest::FetchConstant));
      break;
    case 4:
      if (bit == 7) {
        // The flow constants: bool and loop words of both shaders, one tag.
        copy(offsetof(D3DDevice, m_Constants.VertexShaderB),
             offsetof(D3DDevice, m_ClipPlanes) - offsetof(D3DDevice, m_Constants.VertexShaderB));
      } else if (bit >= 15 && bit <= 17) {
        copy(offsetof(D3DDevice, m_WindowPacket) + 4 * (bit - 15), 4);
      } else if (bit >= 18 && bit <= 25) {
        copy(offsetof(D3DDevice, m_PointPacket) + 4 * (bit - 18), 4);
      } else if (bit >= 26) {
        copy(offsetof(D3DDevice, m_MiscPacket) + 4 * (bit - 26), 4);
      }
      break;
    default: break;
  }
}

// Live device plus the recorded registers, the recorded draw's own index buffer, declaration and stream strides,
// and the blend overrides that apply to this run.
std::vector<uint8_t> Merge(uint32_t live_device, const Entry& e, uint32_t select) {
  std::vector<uint8_t> merged(Guest<uint8_t>(live_device), Guest<uint8_t>(live_device) + kDeviceBytes);
  const uint8_t* snap = e.device.data();
  for (int mask = 0; mask < 5; ++mask) {
    if (!e.tags[mask]) continue;
    for (int bit = 0; bit < 64; ++bit) {
      if (Bit(e.tags, mask, bit)) CopyTag(merged.data(), snap, mask, bit);
    }
  }
  auto copy = [&](size_t offset, size_t size) { std::memcpy(merged.data() + offset, snap + offset, size); };
  copy(offsetof(D3DDevice, index_buffer), 4);
  copy(offsetof(D3DDevice, vertex_declaration), 4);
  copy(offsetof(D3DDevice, stream_stride_dwords), guest::kStreams);
  // Shader binds are tags 2:43 and 2:44 and every avatar recording carries them (the variant masks are zeroed
  // before CreateDrawCommands); copying the pointers outright gives the same result and keeps the replay whole
  // if a recording ever inherits a shader.
  copy(offsetof(D3DDevice, vertex_shader), 4);
  copy(offsetof(D3DDevice, pixel_shader), 4);
  auto* device = reinterpret_cast<D3DDevice*>(merged.data());
  for (const BlendSet& b : e.blends) {
    if (b.run != 0 && b.run != select) continue;
    switch (b.index) {
      case 0: device->m_ControlPacket.BlendControl0.raw = b.blend; break;
      case 1: device->m_ControlPacket.BlendControl1.raw = b.blend; break;
      case 2: device->m_ControlPacket.BlendControl2.raw = b.blend; break;
      case 3: device->m_ControlPacket.BlendControl3.raw = b.blend; break;
      default: break;
    }
  }
  return merged;
}

}  // namespace

bool Recording(uint32_t device) {
  std::lock_guard lock(g_mutex);
  return g_recording.contains(device);
}

void Begin(uint32_t device, uint32_t buffer, uint32_t flags) {
  RecordingState s;
  s.buffer = buffer;
  s.flags = flags;
  ReadPending(device, s.begin_pending);
  std::lock_guard lock(g_mutex);
  g_recording[device] = std::move(s);
}

void End(uint32_t device) {
  std::lock_guard lock(g_mutex);
  auto it = g_recording.find(device);
  if (it == g_recording.end()) return;
  Buffer& b = g_buffers[it->second.buffer];
  b.device = device;
  b.flags = it->second.flags;
  b.entries = std::make_shared<const std::vector<Entry>>(std::move(it->second.entries));
  g_recording.erase(it);
}

void SetPredication(uint32_t device, uint32_t run) {
  std::lock_guard lock(g_mutex);
  if (auto it = g_recording.find(device); it != g_recording.end()) it->second.run = run;
}

bool RecordBlend(uint32_t device, uint32_t index, uint32_t blend) {
  std::lock_guard lock(g_mutex);
  auto it = g_recording.find(device);
  if (it == g_recording.end()) return false;
  it->second.blends.push_back(BlendSet{it->second.run, index, blend});
  return true;
}

void NotePending(uint32_t device) {
  std::lock_guard lock(g_mutex);
  auto it = g_recording.find(device);
  if (it == g_recording.end()) return;
  // BeginCommandBuffer 0x8253BDB0 overwrites the pending masks with ~inherit, so what is pending at a flush is
  // exactly what the recording set since Begin plus the non-inherited registers.
  uint64_t pending[5];
  ReadPending(device, pending);
  for (int i = 0; i < 5; ++i) it->second.tags[i] |= pending[i];
}

void RecordDraw(const draw::DrawArgs& a) {
  std::lock_guard lock(g_mutex);
  auto it = g_recording.find(a.device);
  if (it == g_recording.end()) return;
  if (a.inline_data) {
    if (!g_warn_inline.exchange(true)) REXGPU_WARN("[gpu] a Begin/UP draw inside a command buffer is not replayed");
    return;
  }
  Entry e;
  e.draw = a;
  e.device.assign(Guest<uint8_t>(a.device), Guest<uint8_t>(a.device) + kDeviceBytes);
  e.vs = draw::SnapshotVs(Guest<D3DDevice>(a.device));
  std::memcpy(e.tags, it->second.tags, sizeof(e.tags));
  e.blends = it->second.blends;
  it->second.entries.push_back(std::move(e));
}

void RecordClear(uint32_t device, const capture::ClearArgs& a) {
  std::lock_guard lock(g_mutex);
  auto it = g_recording.find(device);
  if (it == g_recording.end()) return;
  Entry e;
  e.clear = true;
  e.clear_args = a;
  e.device.assign(Guest<uint8_t>(device), Guest<uint8_t>(device) + kDeviceBytes);
  std::memcpy(e.tags, it->second.tags, sizeof(e.tags));
  e.blends = it->second.blends;
  it->second.entries.push_back(std::move(e));
}

void Run(PPCContext& ctx, uint8_t* base, uint32_t device, uint32_t buffer, uint32_t select) {
  std::shared_ptr<const std::vector<Entry>> entries;
  uint32_t recorded_on = 0;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_buffers.find(buffer);
    if (it == g_buffers.end()) {
      if (!g_warn_unknown.exchange(true)) REXGPU_WARN("[gpu] RunCommandBuffer on a buffer with no recording");
      return;
    }
    entries = it->second.entries;
    recorded_on = it->second.device;
  }
  if (!entries) return;
  // The avatar's bone palette (stream 3, fetch constant 92) is CPU-written every frame, so the half the live device
  // points at is dirtied before the replay reads it.
  {
    const auto* live = Guest<D3DDevice>(device);
    const uint32_t palette = uint32_t(live->m_Constants.Fetch[30].dword[4]) & ~3u;
    const uint32_t bytes = uint32_t(live->m_Constants.Fetch[30].dword[5]) & 0x3FFFFFC;
    if (palette && bytes) memory::MarkDirty(palette & 0x1FFFFFFF, bytes);
  }
  for (const Entry& e : *entries) {
    const std::vector<uint8_t> merged = Merge(device, e, select);
    const auto* merged_device = reinterpret_cast<const D3DDevice*>(merged.data());
    if (e.clear) {
      cc2::gpu::Submit(capture::MakeClear(merged_device, e.clear_args));
    } else {
      draw::DrawArgs a = e.draw;
      a.device = recorded_on;
      draw::Replay(ctx, base, a, merged_device, e.vs);
    }
  }
}

}  // namespace cc2::gpu::cmdbuf
