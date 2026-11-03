// D3D command buffers: while a device records, its draws, clears and blend overrides are kept with a copy of the
// device struct and the register tags the recording dirtied; RunCommandBuffer replays them over the live device,
// taking the recorded registers and inheriting everything else.
#pragma once

#include <cstdint>
#include <vector>

#include <rex/ppc/context.h>

#include "gpu/gpu_draw.h"
#include "gpu/gpu_records.h"

namespace cc2::gpu::guest {
struct D3DDevice;
}

namespace cc2::gpu::capture {

// D3DDevice_Clear arguments, kept for a recorded clear.
struct ClearArgs {
  uint32_t flags = 0;
  uint32_t color = 0;
  float z = 1.0f;
  uint32_t stencil = 0;
  std::vector<Rect> rects;
};

// The clear record for these arguments against the targets `device` has bound.
ClearRecord MakeClear(const guest::D3DDevice* device, const ClearArgs& a);

}  // namespace cc2::gpu::capture

namespace cc2::gpu::cmdbuf {

bool Recording(uint32_t device);

// D3DDevice_BeginCommandBuffer / EndCommandBuffer on `device`; a buffer address is reused by re-recording.
void Begin(uint32_t device, uint32_t buffer, uint32_t flags);
void End(uint32_t device);

// D3DDevice_SetCommandBufferPredication: later state carries this run value (0 = every run).
void SetPredication(uint32_t device, uint32_t run);

// GpuSetBlendControl while recording; false when the device is not recording.
bool RecordBlend(uint32_t device, uint32_t index, uint32_t blend);

// Called before the runtime flushes a draw or clear on a recording device: folds the pending register tags in.
void NotePending(uint32_t device);

void RecordDraw(const draw::DrawArgs& a);
void RecordClear(uint32_t device, const capture::ClearArgs& a);

// D3DDevice_RunCommandBuffer on the live `device`.
void Run(PPCContext& ctx, uint8_t* base, uint32_t device, uint32_t buffer, uint32_t select);

}  // namespace cc2::gpu::cmdbuf
