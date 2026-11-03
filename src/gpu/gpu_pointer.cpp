#include "gpu/gpu_pointer.h"
#include "gpu/gpu_texture_decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

namespace cc2::gpu {
namespace {

// Cooked Cal3D mesh: "CHM\0", version, vertex count, face count (big-endian), then 80-byte vertices of
// position xyz + 1, seven unused floats, normal xyz, tangent xyz + 1, uv, and 16-bit face indices.
constexpr uint32_t kVertexBytes = 80;

uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
float BEFloat(const uint8_t* p) {
  const uint32_t u = BE32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out.empty();
}

// The uncompressed CAF root track supplies the glove's translation and loop duration.
void LoadPointerBob(const std::filesystem::path& ui_root, float texels_per_unit, PointerSprite& out) {
  out.bob.clear();
  out.bob_duration = 0;
  std::vector<uint8_t> caf;
  if (!ReadFile(ui_root / "hand_pointer.caf", caf) || caf.size() < 28 ||
      std::memcmp(caf.data(), "CAF\0", 4) != 0) return;
  auto integer = [&](size_t at) {
    return uint32_t(caf[at]) | uint32_t(caf[at + 1]) << 8 | uint32_t(caf[at + 2]) << 16 |
           uint32_t(caf[at + 3]) << 24;
  };
  auto number = [&](size_t at) {
    const uint32_t bits = integer(at);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  };
  if (integer(4) != 1010 || integer(8) != 0 || integer(16) == 0 || integer(20) != 0) return;
  const float duration = number(12);
  const uint32_t count = integer(24);
  if (!std::isfinite(duration) || duration <= 0 || count < 2 || count > (caf.size() - 28) / 32) return;
  const float origin = number(32);
  std::vector<PointerBobKey> keys;
  for (uint32_t i = 0; i < count; ++i) {
    const float time = number(28 + size_t(i) * 32);
    const float offset = (number(32 + size_t(i) * 32) - origin) * texels_per_unit;
    if (!std::isfinite(time) || !std::isfinite(offset) || time < 0 || time > duration ||
        (!keys.empty() && time <= keys.back().time)) return;
    keys.push_back({time, offset});
  }
  if (keys.front().time != 0 || keys.back().time != duration) return;
  out.bob_duration = duration;
  out.bob = std::move(keys);
}

struct Vertex {
  float x, y, z, nx, ny, nz, u, v, model_x, model_z;
};

// A DXT1 or DXT5 DDS without mips, decoded to straight RGBA for the software sampler.
bool DecodeAtlas(const std::vector<uint8_t>& dds, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
  if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0) return false;
  std::memcpy(&h, &dds[12], 4);
  std::memcpy(&w, &dds[16], 4);
  const bool dxt1 = std::memcmp(&dds[84], "DXT1", 4) == 0, dxt5 = std::memcmp(&dds[84], "DXT5", 4) == 0;
  if (!w || !h || (w & 3) || (h & 3) || (!dxt1 && !dxt5)) return false;
  const uint32_t bx = w / 4, by = h / 4, block = dxt1 ? 8 : 16;
  if (dds.size() < 128 + size_t(bx) * by * block) return false;
  rgba.assign(size_t(w) * h * 4, 0);
  uint8_t tile[16][4];
  for (uint32_t y = 0; y < by; ++y) {
    for (uint32_t x = 0; x < bx; ++x) {
      DecodeBlockRGBA(dxt1 ? HostFormat::kBC1 : HostFormat::kBC3, &dds[128 + (size_t(y) * bx + x) * block], tile);
      for (uint32_t i = 0; i < 16; ++i) {
        std::memcpy(&rgba[(size_t(y * 4 + i / 4) * w + x * 4 + i % 4) * 4], tile[i], 4);
      }
    }
  }
  return true;
}

}  // namespace

float PointerBobOffset(const PointerSprite& sprite, double seconds) {
  if (sprite.bob.empty() || sprite.bob_duration <= 0 || !std::isfinite(seconds) || seconds < 0) return 0;
  const float time = float(std::fmod(seconds, double(sprite.bob_duration)));
  for (size_t i = 1; i < sprite.bob.size(); ++i) {
    const auto& a = sprite.bob[i - 1];
    const auto& b = sprite.bob[i];
    if (time <= b.time) return std::lerp(a.offset, b.offset, (time - a.time) / (b.time - a.time));
  }
  return sprite.bob.front().offset;
}

bool LoadSelectorSprite(const std::filesystem::path& ui_root, float angle_degrees, PointerSprite& out) {
  std::vector<uint8_t> dds, sheet;
  uint32_t sw = 0, sh = 0;
  if (!ReadFile(ui_root / "men" / "hand_selector.dds", dds) || !DecodeAtlas(dds, sheet, sw, sh)) return false;
  // The top frame of the sheet: a square of the sheet's width.
  const uint32_t fw = sw, fh = std::min(sw, sh);
  // Rotated into a canvas large enough for any angle, with bilinear taps in premultiplied space so the
  // edge keeps its coverage.
  const uint32_t size = uint32_t(std::ceil(std::sqrt(float(fw * fw + fh * fh)))) + 2;
  const float a = angle_degrees * 3.14159265f / 180.0f;
  const float c = std::cos(a), s = std::sin(a);
  const float cx = float(size) * 0.5f, cy = float(size) * 0.5f, fx = float(fw) * 0.5f, fy = float(fh) * 0.5f;
  out.width = out.height = size;
  out.rgba.assign(size_t(size) * size * 4, 0);
  int best = std::numeric_limits<int>::max();
  for (uint32_t y = 0; y < size; ++y) {
    for (uint32_t x = 0; x < size; ++x) {
      // Inverse rotation: canvas texel back into the frame.
      const float dx = float(x) + 0.5f - cx, dy = float(y) + 0.5f - cy;
      const float u = dx * c + dy * s + fx - 0.5f, v = -dx * s + dy * c + fy - 0.5f;
      const int u0 = int(std::floor(u)), v0 = int(std::floor(v));
      const float tu = u - float(u0), tv = v - float(v0);
      float acc[4] = {0, 0, 0, 0};
      for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
          const int su = u0 + i, sv = v0 + j;
          if (su < 0 || sv < 0 || su >= int(fw) || sv >= int(fh)) continue;
          const float wgt = (i ? tu : 1.0f - tu) * (j ? tv : 1.0f - tv);
          const uint8_t* t = &sheet[(size_t(sv) * sw + su) * 4];
          const float alpha = float(t[3]) / 255.0f;
          for (int k = 0; k < 3; ++k) acc[k] += float(t[k]) * alpha * wgt;
          acc[3] += alpha * wgt;
        }
      }
      uint8_t* dst = &out.rgba[(size_t(y) * size + x) * 4];
      if (acc[3] > 0.0f) {
        for (int k = 0; k < 3; ++k) dst[k] = uint8_t(std::min(255.0f, acc[k] / acc[3]));
        dst[3] = uint8_t(std::min(255.0f, acc[3] * 255.0f));
        if (dst[3] > 200 && int(x + y) < best) {
          best = int(x + y);
          out.hotspot_x = int(x);
          out.hotspot_y = int(y);
        }
      }
    }
  }
  return true;
}

bool BakePointerSprite(const std::filesystem::path& ui_root, uint32_t size, float angle_degrees, PointerSprite& out) {
  std::vector<uint8_t> mesh, dds, atlas;
  if (!ReadFile(ui_root / "hand_pointer.cmfh", mesh) || !ReadFile(ui_root / "hand_pointer.dds", dds)) return false;
  if (mesh.size() < 16 || std::memcmp(mesh.data(), "CHM", 3) != 0) return false;
  const uint32_t vertex_count = BE32(&mesh[8]), face_count = BE32(&mesh[12]);
  const size_t index_at = 16 + size_t(vertex_count) * kVertexBytes;
  if (mesh.size() < index_at + size_t(face_count) * 6) return false;
  uint32_t aw = 0, ah = 0;
  if (!DecodeAtlas(dds, atlas, aw, ah)) return false;

  // The glove seen from its back (x right, z down on screen, -y toward the viewer), turned clockwise on
  // screen from the model's left-pointing finger; -80 matches the game's own tutorial hand.
  const float c = std::cos(angle_degrees * 3.14159265f / 180.0f), s = std::sin(angle_degrees * 3.14159265f / 180.0f);
  std::vector<Vertex> verts(vertex_count);
  float minx = std::numeric_limits<float>::max(), miny = minx, maxx = -minx, maxy = -minx;
  for (uint32_t i = 0; i < vertex_count; ++i) {
    const uint8_t* p = &mesh[16 + size_t(i) * kVertexBytes];
    const float mx = BEFloat(p), my = BEFloat(p + 4), mz = BEFloat(p + 8);
    const float nx = BEFloat(p + 44), ny = BEFloat(p + 48), nz = BEFloat(p + 52);
    Vertex& v = verts[i];
    v.model_x = mx;
    v.model_z = mz;
    const float sx = mx, sy = mz;
    v.x = sx * c - sy * s;
    v.y = sx * s + sy * c;
    v.z = my;  // smaller is nearer
    const float snx = nx, sny = nz;
    v.nx = snx * c - sny * s;
    v.ny = snx * s + sny * c;
    v.nz = ny;
    v.u = BEFloat(p + 72);
    v.v = BEFloat(p + 76);
    minx = std::min(minx, v.x);
    maxx = std::max(maxx, v.x);
    miny = std::min(miny, v.y);
    maxy = std::max(maxy, v.y);
  }
  // Rendered at twice the size and box-filtered down, which gives the edge its coverage alpha.
  const uint32_t big = size * 2;
  const float margin = 4.0f;
  const float scale = (float(big) - 2.0f * margin) / std::max(maxx - minx, maxy - miny);
  for (Vertex& v : verts) {
    v.x = (v.x - minx) * scale + margin;
    v.y = (v.y - miny) * scale + margin;
  }
  std::vector<uint8_t> color(size_t(big) * big * 4, 0);
  std::vector<float> depth(size_t(big) * big, std::numeric_limits<float>::max());
  // Preserve the fitted diffuse and rim lighting; the back of the glove receives a separate tight reflection.
  const float lx = 0.0f, ly = -0.05f, lz = -0.9987f;
  const float hx = lx * 0.5f, hy = ly * 0.5f, hz = (lz - 1.0f) * 0.5f;  // half vector with the viewer at -z
  const float hl = std::sqrt(hx * hx + hy * hy + hz * hz);
  const uint8_t* indices = &mesh[index_at];
  for (uint32_t f = 0; f < face_count; ++f) {
    const uint32_t a = uint32_t(indices[f * 6]) << 8 | indices[f * 6 + 1];
    const uint32_t b = uint32_t(indices[f * 6 + 2]) << 8 | indices[f * 6 + 3];
    const uint32_t d = uint32_t(indices[f * 6 + 4]) << 8 | indices[f * 6 + 5];
    if (a >= vertex_count || b >= vertex_count || d >= vertex_count) continue;
    const Vertex &v0 = verts[a], &v1 = verts[b], &v2 = verts[d];
    const float det = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    if (std::fabs(det) < 1e-6f) continue;
    const int x0 = std::max(0, int(std::floor(std::min({v0.x, v1.x, v2.x}))));
    const int x1 = std::min(int(big) - 1, int(std::ceil(std::max({v0.x, v1.x, v2.x}))));
    const int y0 = std::max(0, int(std::floor(std::min({v0.y, v1.y, v2.y}))));
    const int y1 = std::min(int(big) - 1, int(std::ceil(std::max({v0.y, v1.y, v2.y}))));
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        const float px = float(x) + 0.5f, py = float(y) + 0.5f;
        const float w1 = ((px - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (py - v0.y)) / det;
        const float w2 = ((v1.x - v0.x) * (py - v0.y) - (px - v0.x) * (v1.y - v0.y)) / det;
        const float w0 = 1.0f - w1 - w2;
        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
        const float z = w0 * v0.z + w1 * v1.z + w2 * v2.z;
        float& zb = depth[size_t(y) * big + x];
        if (z >= zb) continue;
        zb = z;
        float nx = w0 * v0.nx + w1 * v1.nx + w2 * v2.nx;
        float ny = w0 * v0.ny + w1 * v1.ny + w2 * v2.ny;
        float nz = w0 * v0.nz + w1 * v1.nz + w2 * v2.nz;
        const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nl > 0.0f) nx /= nl, ny /= nl, nz /= nl;
        const float u = w0 * v0.u + w1 * v1.u + w2 * v2.u, v = w0 * v0.v + w1 * v1.v + w2 * v2.v;
        const int tx = int(std::floor(u * float(aw))) % int(aw), ty = int(std::floor(v * float(ah))) % int(ah);
        const uint8_t* texel = &atlas[(size_t(ty < 0 ? ty + int(ah) : ty) * aw + (tx < 0 ? tx + int(aw) : tx)) * 4];
        const float diffuse = std::max(0.0f, nx * lx + ny * ly + nz * lz);
        const float facing = std::max(0.0f, -nz);
        const float reflection = std::max(0.0f, (nx * hx + ny * hy + nz * hz) / hl);
        const float model_x = w0 * v0.model_x + w1 * v1.model_x + w2 * v2.model_x;
        const float finger = std::clamp((-model_x - 0.25f) / 0.30f, 0.0f, 1.0f);
        const float spec = std::pow(reflection, 28.0f) * (1.0f - finger);
        const float hand_x = (model_x - 0.05f) / 0.50f;
        const float hand_z = (w0 * v0.model_z + w1 * v1.model_z + w2 * v2.model_z + 0.02f) / 0.32f;
        const float hand_mask = std::clamp(1.0f - hand_x * hand_x - hand_z * hand_z, 0.0f, 1.0f);
        const float glare = std::pow(reflection, 160.0f) * hand_mask;
        const float rim = std::pow(1.0f - facing, 3.0f);
        const float shade = 1.15f * (0.65f + 0.40f * diffuse);
        const float add = 90.0f * spec + 230.0f * glare + 73.0f * rim;
        uint8_t* dst = &color[(size_t(y) * big + x) * 4];
        for (int k = 0; k < 3; ++k) dst[k] = uint8_t(std::min(255.0f, float(texel[k]) * shade + add));
        dst[3] = 255;
      }
    }
  }
  out.width = out.height = size;
  out.rgba.assign(size_t(size) * size * 4, 0);
  // The fingertip is the opaque texel furthest along the finger's direction, the model's -x turned by the angle.
  const float dir_x = -c, dir_y = -s;
  out.bob_direction_x = dir_x;
  out.bob_direction_y = dir_y;
  float best = -std::numeric_limits<float>::max();
  for (uint32_t y = 0; y < size; ++y) {
    for (uint32_t x = 0; x < size; ++x) {
      uint32_t sum[4] = {0, 0, 0, 0};
      for (int sy = 0; sy < 2; ++sy) {
        for (int sx = 0; sx < 2; ++sx) {
          const uint8_t* src = &color[(size_t(y * 2 + sy) * big + x * 2 + sx) * 4];
          if (!src[3]) continue;
          for (int k = 0; k < 3; ++k) sum[k] += src[k];
          sum[3] += 1;
        }
      }
      uint8_t* dst = &out.rgba[(size_t(y) * size + x) * 4];
      if (sum[3]) {
        for (int k = 0; k < 3; ++k) dst[k] = uint8_t(sum[k] / sum[3]);
        dst[3] = uint8_t(sum[3] * 255 / 4);
        const float along = float(x) * dir_x + float(y) * dir_y;
        if (sum[3] == 4 && along > best) {
          best = along;
          out.hotspot_x = int(x);
          out.hotspot_y = int(y);
        }
      }
    }
  }
  LoadPointerBob(ui_root, scale * 0.5f, out);
  return true;
}

}  // namespace cc2::gpu
