#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace cc2::gpu {

struct PointerBobKey {
  float time, offset;
};

// The mouse hand is baked from pointer0 assets, with its hotspot on the fingertip.
struct PointerSprite {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;  // straight alpha, row-major
  int hotspot_x = 0, hotspot_y = 0;
  float bob_duration = 0;
  float bob_direction_x = 0, bob_direction_y = 0;
  std::vector<PointerBobKey> bob;
};

// Samples the native glove bob in sprite texels; the input hotspot stays fixed.
float PointerBobOffset(const PointerSprite& sprite, double seconds);

// ui_root is .../data/art/common/ui. size is the sprite's edge in texels; false when the assets are missing.
bool BakePointerSprite(const std::filesystem::path& ui_root, uint32_t size, float angle_degrees, PointerSprite& out);

// The menu's own focus hand, men/hand_selector.dds (a DXT5 sheet of two frames; the top one points left),
// turned by angle_degrees clockwise so the finger points where a cursor should. Pixel-identical to the
// sprite the GUI draws, before the frame's own scaling.
bool LoadSelectorSprite(const std::filesystem::path& ui_root, float angle_degrees, PointerSprite& out);

}  // namespace cc2::gpu
