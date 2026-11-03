#pragma once

#include <bit>
#include <cstdint>

namespace cc2::gpu::edge {

// TU1 0x82545CAC..0x82545DC4: preflight the ALU flush without changing the device.
// Leave split, wrap and unaligned paths to the runtime.
inline bool AluRingCursor(uint64_t mask, uint32_t cursor, uint32_t limit,
                          uint32_t& result) {
  if (!mask || (cursor & 3)) return false;
  while (mask) {
    mask <<= std::countl_zero(mask);
    const unsigned run = std::countl_one(mask);
    const uint64_t checked_end = uint64_t(cursor) + run * 64 + 12;
    if (checked_end >= limit) return false;
    const uint32_t padding = (8 - cursor) & 15;
    const uint64_t next = uint64_t(cursor) + padding + 4 + run * 64;
    if (next > UINT32_MAX) return false;
    cursor = uint32_t(next);
    mask = run == 64 ? 0 : mask << run;
  }
  result = cursor;
  return true;
}

}  // namespace cc2::gpu::edge
