#pragma once

#include <cstdint>

#include <rex/ui/overlay/debug_overlay.h>

namespace cc2::scene {

// Call once per presented frame, from whatever owns the swap.
void OnSwap();

// Call once per D3D draw call, from whatever owns the draw entry points.
void OnDraw();

// D3D draw calls issued in the last completed frame.
uint32_t LastFrameDraws();

// Frames presented since launch.
uint64_t FrameCount();

// Presented-frame rate measured at the swap, for the F3 overlay.
rex::ui::FrameStats GuestFrameStats();

}  // namespace cc2::scene
