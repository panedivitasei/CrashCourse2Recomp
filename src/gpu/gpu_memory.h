// Guest-thread tracking of GPU-visible guest memory: vertex, index and texture ranges registered by the D3D and XG
// lifetime functions, dirtied by Lock and header changes. Engine CPU writers are not hooked.
#pragma once

#include <cstdint>
#include <memory>

namespace cc2::gpu::memory {

struct RangeState {
  bool registered = false;   // inside a range a lifetime hook registered
  uint32_t generation = 0;   // changes on every dirty event touching the range
};

struct Range;
struct QueryCache {
  uint64_t key = 0;
  uint64_t revision = 0;
  std::shared_ptr<Range> range;
};

void Register(uint32_t base, uint32_t size);
void MarkDirty(uint32_t base, uint32_t size);
RangeState Query(uint32_t base, uint32_t size);
RangeState Query(uint32_t base, uint32_t size, QueryCache& cached);

// Registers or dirties the data range of a D3D resource header (vertex buffer, index buffer or texture).
void RegisterResource(uint32_t resource_address);
void DirtyResource(uint32_t resource_address);

}  // namespace cc2::gpu::memory
