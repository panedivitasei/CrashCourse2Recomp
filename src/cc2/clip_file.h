#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// A saved game clip: the game's "CC2LbClip5" upload followed by a trailer naming the course and the player.
namespace cc2::clips {

// lzx_decompress(data, size, out, out size, window size, window data, window data size): 0 on success.
using Inflate = int (*)(const void*, size_t, void*, size_t, uint32_t, void*, size_t);

constexpr size_t kHeaderSize = 48;         // magic, raw size and SHA-256
constexpr size_t kTrailerSize = 28;        // "CC2CLIP1", course (LevelId, big-endian), name[16]
constexpr size_t kMaxFileSize = 256 * 1024;
constexpr size_t kMaxRawSize = 1024 * 1024;
constexpr size_t kTagSize = 15;            // a gamertag, less its terminator
constexpr size_t kAvatarSize = 1012;       // X_AVATAR_METADATA, award title and id, vanity id
constexpr size_t kManifestSize = 1000;     // X_AVATAR_METADATA
constexpr size_t kRawFixed = 1177;         // time, flag, avatar, 16 checkpoints twice, three stream sizes, five ids
constexpr uint32_t kFrame = 0x8000, kWindow = 0x20000;
constexpr uint32_t kMinTime = 20000, kMaxTime = 5999999;  // the game posts no faster run; 99:59.999
constexpr int kCourses = 35;
constexpr char kMagic[] = "CC2LbClip5", kTrailerMagic[] = "CC2CLIP1", kExtension[] = ".clip";
constexpr char kSalt[] = "-C[cOfd-zIcFq_m4gB`)zhV/DrCVe&dY^!bqoayRHCeaqd\"G:V*[U!_h";  // hashed with its terminator

// Courses in leaderboard page order, seven per world.
constexpr const char* kCourseNames[kCourses] = {
    "JungleGymAntics",     "HitTheRopeJack",   "HeavyHammerHop",  "TrapolineTrials",   "HammeringHeadway",
    "MayanMotorsInc",      "Incapalooza",      "SnowflakeShake",  "GoWithTheFloe",     "Shockwave",
    "SeesawSaunter",       "IcebergAlley",     "FrostbiteFlurry", "DoubleTrouble",     "SandShenanigans",
    "YourBiggestFan",      "HangTime",         "IveGotABadFeeling", "PharaohsCurse",   "FlipFlopFloor",
    "GlassAspCanter",      "OMGFlyingPirates", "YoHoHeaveHo",     "WalkinDaPlank",     "MelbourneScuttle",
    "BarrelBonanza",       "SeaShantyStride",  "BeyondTheSea",    "TeleporterMayhem",  "LuckyFortuneWheel",
    "PatienceGrasshopper", "OneInchPunch",     "ILoveLuckyCat",   "HoverHeliHustle",   "BackstrokeOfTheEast"};

inline uint32_t LevelId(int course) { return uint32_t(course / 7 + 1) * 1000 + uint32_t(course % 7) * 100; }

inline int CourseOfLevel(uint32_t level) {
  const uint32_t world = level / 1000, track = level % 1000 / 100;
  return world >= 1 && world <= 5 && track < 7 && level % 100 == 0 ? int((world - 1) * 7 + track) : -1;
}

// A course's all-time or weekly stats view: twelve ids from a base per world, then the second bonus course's pair.
inline int CourseOfView(uint32_t view) {
  constexpr uint32_t kBase[5] = {0, 36, 12, 24, 48}, kBonus[5] = {61, 67, 63, 65, 69};
  for (int world = 0; world < 5; ++world) {
    if (view >= kBase[world] && view < kBase[world] + 12) return world * 7 + int(view - kBase[world]) % 6;
    if (view == kBonus[world] || view == kBonus[world] + 1) return world * 7 + 6;
  }
  return -1;
}

inline uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

class Sha256 {
 public:
  void Add(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
      block_[fill_++] = data[i];
      if (fill_ == 64) Block();
    }
    total_ += size;
  }
  void Finish(uint8_t* out) {
    const uint64_t bits = total_ * 8;
    block_[fill_++] = 0x80;
    if (fill_ > 56) {
      std::memset(block_ + fill_, 0, 64 - fill_);
      Block();
    }
    std::memset(block_ + fill_, 0, 56 - fill_);
    for (int i = 0; i < 8; ++i) block_[56 + i] = uint8_t(bits >> (56 - 8 * i));
    Block();
    for (int i = 0; i < 8; ++i) PutBe32(out + 4 * i, h_[i]);
  }

 private:
  static uint32_t Ror(uint32_t v, int n) { return v >> n | v << (32 - n); }
  void Block() {
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; ++i) w[i] = Be32(block_ + 4 * i);
    for (int i = 16; i < 64; ++i) {
      w[i] = w[i - 16] + (Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
             (Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
    }
    std::memcpy(s, h_, sizeof(s));
    for (int i = 0; i < 64; ++i) {
      const uint32_t a = s[7] + (Ror(s[4], 6) ^ Ror(s[4], 11) ^ Ror(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) +
                         k[i] + w[i];
      const uint32_t b =
          (Ror(s[0], 2) ^ Ror(s[0], 13) ^ Ror(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
      std::memmove(s + 1, s, 7 * sizeof(uint32_t));
      s[4] += a;
      s[0] = a + b;
    }
    for (int i = 0; i < 8; ++i) h_[i] += s[i];
    fill_ = 0;
  }
  uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t block_[64];
  size_t fill_ = 0;
  uint64_t total_ = 0;
};

// The header's hash: SHA-256 of the compressed stream, the raw size field and the salt.
inline void Hash(const uint8_t* blob, size_t size, uint8_t* out) {
  Sha256 sha;
  sha.Add(blob + kHeaderSize, size - kHeaderSize);
  sha.Add(blob + 12, 4);
  sha.Add(reinterpret_cast<const uint8_t*>(kSalt), sizeof(kSalt));
  sha.Finish(out);
}

// An X_AVATAR_METADATA shaped like the ones the console writes, so the avatar builder gets no slot it was not made for.
inline bool Manifest(const uint8_t* m) {
  static constexpr uint8_t kStock[8] = {0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0};
  const auto zero = [&](size_t at) { return std::all_of(m + at, m + at + 16, [](uint8_t b) { return !b; }); };
  // A stock pack id whose first field, the asset's kind, lies inside 'kinds'.
  const auto stock = [&](size_t at, uint32_t kinds) {
    return !std::memcmp(m + at + 8, kStock, 8) && Be32(m + at) && !(Be32(m + at) & ~kinds);
  };
  const auto real = [&](size_t at, float limit) {
    const uint32_t bits = Be32(m + at);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value) && std::fabs(value) <= limit;
  };
  if (!real(4, 1.0f) || !real(8, 1.0f)) return false;  // weight and height
  for (size_t at = 0x0C; at < 0x3C; at += 16) {  // three face shapes
    if (!zero(at) && !stock(at, 0x380000)) return false;
  }
  for (size_t at = 0x3C; at < 0xFC; at += 32) {  // six face textures, each with a placement
    if (!zero(at) && !stock(at, 0x7E000)) return false;
    for (size_t f = 16; f < 32; f += 4) {
      if (!real(at + f, 1e6f)) return false;
    }
  }
  const auto categories = [&](size_t at) { return uint32_t(m[at + 16]) << 8 | m[at + 17]; };
  if (!stock(0x120, 2) || categories(0x120) != 2 || !stock(0x140, 1) || categories(0x140) != 1) return false;
  uint32_t worn = 0;
  for (size_t at = 0x160; at < 0x300; at += 32) {  // thirteen worn items: one per category, never head or body
    if (zero(at)) continue;
    const uint32_t c = categories(at);
    if (!c || (c & ~0x1FFCu) || (Be32(m + at) & 0xFFFF) != c || (c & worn)) return false;
    worn |= c;
  }
  for (size_t at = 0x300; at < 0x380; at += 32) {  // four fallbacks: hair, top, bottom, shoes
    if (zero(at)) continue;
    const uint32_t c = categories(at);
    if ((c != 4 && c != 8 && c != 16 && c != 32) || (Be32(m + at) & 0xFFFF) != c) return false;
  }
  return true;
}

// Checks everything GameClipPlayer::DecompressBuffer trusts in a "CC2LbClip5" blob; reads its time (ms) and avatar.
inline bool Native(const uint8_t* blob, size_t size, Inflate inflate, uint32_t* time, uint8_t* avatar = nullptr) {
  if (size < kHeaderSize + 11 || size > kMaxFileSize || std::memcmp(blob, kMagic, 10)) return false;
  const uint32_t raw_size = Be32(blob + 12);
  if (raw_size < kRawFixed || raw_size > kMaxRawSize) return false;
  uint8_t hash[32];
  Hash(blob, size, hash);
  if (std::memcmp(hash, blob + 16, sizeof(hash))) return false;
  // XMemCompress frames: a size before each 32 KB frame, 0xFF and both sizes before the last, five zeros after.
  std::vector<uint8_t> lzx;
  lzx.reserve(size);
  const size_t end = size - 5;
  size_t at = kHeaderSize, produced = 0;
  for (bool last = false; !last;) {
    if (end - at < 2) return false;
    uint32_t raw = kFrame, packed;
    last = blob[at] == 0xFF;
    if (last) {
      if (end - at < 5) return false;
      raw = uint32_t(blob[at + 1]) << 8 | blob[at + 2];
      packed = uint32_t(blob[at + 3]) << 8 | blob[at + 4];
      at += 5;
    } else {
      packed = uint32_t(blob[at]) << 8 | blob[at + 1];
      at += 2;
    }
    if (!raw || raw > kFrame || !packed || packed > end - at) return false;
    lzx.insert(lzx.end(), blob + at, blob + at + packed);
    at += packed;
    produced += raw;
    if (produced > raw_size) return false;
  }
  if (at != end || produced != raw_size) return false;
  std::vector<uint8_t> raw(raw_size);
  if (inflate(lzx.data(), lzx.size(), raw.data(), raw.size(), kWindow, nullptr, 0)) return false;
  *time = Be32(raw.data());
  if (*time < kMinTime || *time > kMaxTime) return false;
  // An avatar with a Fame Star award sends the loader to Sentient for it.
  if (Be32(raw.data() + 5 + 1000) || !Manifest(raw.data() + 5)) return false;
  if (avatar) std::memcpy(avatar, raw.data() + 5, kManifestSize);
  const uint8_t* marks = raw.data() + 5 + kAvatarSize;
  const uint64_t bits = uint64_t(Be32(marks + 128)) * 8;  // the first stream, the run's samples, comes next
  if (!bits) return false;
  for (int i = 0; i < 16; ++i) {
    const uint32_t bit = Be32(marks + 4 * i), seconds = Be32(marks + 64 + 4 * i);
    float value;
    std::memcpy(&value, &seconds, sizeof(value));
    if ((bit != 0xFFFFFFFFu && bit > bits) || !std::isfinite(value)) return false;
  }
  // Three streams, each a size and that many bytes, then the five powerup ids.
  size_t offset = 5 + kAvatarSize + 128;
  for (int stream = 0; stream < 3; ++stream) {
    if (raw_size - offset < 4) return false;
    const uint32_t bytes = Be32(raw.data() + offset);
    if (bytes > raw_size - offset - 4) return false;
    offset += 4 + bytes;
  }
  return raw_size - offset == 20;
}

struct Clip {
  int course = -1;
  uint32_t time = 0;       // milliseconds
  std::string name;        // the player, at most kTagSize characters
  uint64_t key = 0;        // the first hash bytes: one value per recording
  std::array<uint8_t, kManifestSize> avatar{};  // the player's X_AVATAR_METADATA as recorded
  size_t native_size = 0;  // the file without its trailer
};

// The characters of a name: the game draws it as rich text, where others are markup.
inline bool TagChar(char c) {
  return ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
}

// A clip file: the game's blob and the trailer.
inline bool Parse(const uint8_t* file, size_t size, Inflate inflate, Clip* clip) {
  if (size < kHeaderSize + kTrailerSize || size > kMaxFileSize) return false;
  const uint8_t* trailer = file + size - kTrailerSize;
  if (std::memcmp(trailer, kTrailerMagic, 8)) return false;
  clip->course = CourseOfLevel(Be32(trailer + 8));
  if (clip->course < 0) return false;
  const char* name = reinterpret_cast<const char*>(trailer + 12);
  size_t length = 0;
  while (length < 16 && name[length]) ++length;
  if (!length || length > kTagSize) return false;
  for (size_t i = 0; i < 16; ++i) {
    if (i < length ? !TagChar(name[i]) : name[i] != 0) return false;
  }
  clip->name.assign(name, length);
  clip->native_size = size - kTrailerSize;
  if (!Native(file, clip->native_size, inflate, &clip->time, clip->avatar.data())) return false;
  clip->key = uint64_t(Be32(file + 16)) << 32 | Be32(file + 20);
  return true;
}

// A player name as the trailer and the strip hold it: kTagSize of the name's characters.
inline std::string Tag(const std::string& name) {
  std::string tag;
  for (const char c : name) {
    if (TagChar(c) && tag.size() < kTagSize) tag += c;
  }
  while (!tag.empty() && tag.back() == ' ') tag.pop_back();
  return tag.empty() ? "Player" : tag;
}

inline std::vector<uint8_t> Pack(const uint8_t* native, size_t size, int course, const std::string& name) {
  std::vector<uint8_t> file(native, native + size);
  file.resize(size + kTrailerSize, 0);
  uint8_t* trailer = file.data() + size;
  std::memcpy(trailer, kTrailerMagic, 8);
  PutBe32(trailer + 8, LevelId(course));
  const std::string tag = Tag(name);
  std::memcpy(trailer + 12, tag.data(), tag.size());
  return file;
}

// "<player>_<course>_<date>_": a file name up to its number, the player reduced to letters, digits, '-' and '_'.
inline std::string Stem(const std::string& player, int course, const std::string& date) {
  std::string safe;
  for (const char c : player) {
    if (TagChar(c) && c != ' ') safe += c;
  }
  if (safe.empty()) safe = "Player";
  if (safe.size() > 32) safe.resize(32);
  return safe + "_" + kCourseNames[course] + "_" + date + "_";
}

// The first number from 1 that no file in the folder has after the stem; names compare without case, as on Windows.
inline uint32_t NextNumber(const std::vector<std::string>& files, const std::string& stem) {
  const auto lower = [](std::string s) {
    for (char& c : s) c = c >= 'A' && c <= 'Z' ? char(c + 32) : c;
    return s;
  };
  const std::string prefix = lower(stem), suffix = kExtension;
  std::vector<uint32_t> used;
  for (const std::string& file : files) {
    const std::string name = lower(file);
    if (name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix)) {
      continue;
    }
    const std::string digits = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (digits.size() > 9 || digits.find_first_not_of("0123456789") != std::string::npos) continue;
    used.push_back(uint32_t(std::stoul(digits)));
  }
  uint32_t number = 1;
  while (std::find(used.begin(), used.end(), number) != used.end()) ++number;
  return number;
}

}  // namespace cc2::clips
