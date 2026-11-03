#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include "cc2/kernel/xam/lg_economy_file.h"

namespace cc2::fame {
struct Definition {
  uint32_t id;
  const char* text;
  bool weekly;
  std::array<uint32_t, 3> targets;
};
// Conditions and IDs are native; tiers, points, and this weekly selection are reconstructed.
inline constexpr std::array<Definition, 20> kDefinitions{{
  {4, "Complete course bonus objectives", false, {1, 5, 10}},
  {6, "Earn gold medals", false, {1, 5, 10}},
  {9, "Collect stars during races", false, {25, 100, 250}},
  {11, "Jungle: finish with 12 stars and silver or better", false, {1, 3, 10}},
  {12, "Arctic: finish with 10 stars and silver or better", false, {1, 3, 10}},
  {13, "Desert: finish with 8 stars and silver or better", false, {1, 3, 10}},
  {14, "Island: finish with 10 stars and bronze or better", false, {1, 3, 10}},
  {15, "Hong Kong: finish with 8 stars and bronze or better", false, {1, 3, 10}},
  {53, "Perform 20 slide jumps in one race", false, {1, 1, 1}},
  {57, "Chain 5 jumps in one race", false, {1, 1, 1}},
  {21, "Desert: beat the gold time by 4 seconds", true, {1, 3, 5}},
  {22, "Island: beat the gold time by 3 seconds", true, {1, 3, 5}},
  {23, "Hong Kong: beat the gold time by 2 seconds", true, {1, 3, 5}},
  {24, "Jungle: beat the gold time by 6 seconds", true, {1, 3, 5}},
  {25, "Arctic: beat the gold time by 5 seconds", true, {1, 3, 5}},
  {36, "Arctic or Jungle: finish without grabbing a ledge", true, {1, 3, 5}},
  {37, "Jungle or Island: finish without grabbing a ledge", true, {1, 3, 5}},
  {51, "Chain 10 angled-trampoline bounces", true, {1, 1, 1}},
  {59, "Collect 3 stars between checkpoints in a race", true, {1, 3, 5}},
  {60, "Collect 4 stars between checkpoints in a race", true, {1, 3, 5}},
}};
inline constexpr std::array<uint32_t, 3> kTierPoints{25, 50, 100};
inline constexpr std::array<uint32_t, 6> kRankThresholds{0, 500, 1250, 2250, 3500, 5500};
inline constexpr std::array<const char*, 6> kRankNames{
    "Newcomer", "Rising Star", "Star", "Superstar", "Celebrity", "Megastar"};
inline const Definition* Find(uint32_t id) {
  for (const auto& d : kDefinitions) if (d.id == id) return &d;
  return nullptr;
}
inline uint32_t TierCount(const Definition& d) {
  return d.targets[0] == d.targets[2] ? 1u : 3u;
}
inline uint32_t Earned(const Definition& d, uint32_t progress) {
  uint32_t points = 0;
  for (uint32_t i = 0; i < TierCount(d); ++i)
    if (progress >= d.targets[i]) points += kTierPoints[i];
  return points;
}
inline uint32_t Week(uint64_t seconds) {
  return static_cast<uint32_t>((seconds + 3 * 86400ull) / 604800ull);
}
struct State {
  uint32_t week = 0;
  uint32_t acknowledged_rank = 0;
  // Period zero holds lifetime challenges; weekly records survive rollover for point accounting.
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> progress;
  bool Advance(uint32_t now) {
    if (now <= week) return false;
    week = now;
    return true;
  }
  uint32_t Count(const Definition& d) const {
    const auto it = progress.find({d.weekly ? week : 0u, d.id});
    return it == progress.end() ? 0u : it->second;
  }
  uint32_t Points() const {
    uint64_t total = 0;
    for (const auto& [key, count] : progress)
      if (const auto* d = Find(key.second)) total += Earned(*d, count);
    return static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX));
  }
  uint32_t Rank() const {
    uint32_t level = 0;
    while (level + 1 < kRankThresholds.size() && Points() >= kRankThresholds[level + 1]) ++level;
    return level;
  }
  uint32_t Add(uint32_t id, uint32_t delta) {
    const auto* d = Find(id);
    if (!d || !delta) return 0;
    auto& count = progress[{d->weekly ? week : 0u, id}];
    const uint32_t before = Earned(*d, count);
    count = static_cast<uint32_t>(std::min<uint64_t>(uint64_t(count) + delta, d->targets[2]));
    return Earned(*d, count) - before;
  }
  std::string Encode() const {
    std::ostringstream out;
    out << "CC2_FAMESTAR 1 " << week << ' ' << acknowledged_rank << ' ' << progress.size() << '\n';
    for (const auto& [key, count] : progress)
      out << key.first << ' ' << key.second << ' ' << count << '\n';
    out << "END\n";
    return out.str();
  }
  bool Decode(const std::string& data) {
    std::istringstream in(data);
    std::string magic, token;
    auto number = [&](uint32_t& out) {
      return bool(in >> token) && kernel::xam::LgEconomyFile::ReadUInt(token, out);
    };
    State next;
    uint32_t version, size;
    if (!(in >> magic) || magic != "CC2_FAMESTAR" || !number(version) || version != 1 ||
        !number(next.week) || !next.week || !number(next.acknowledged_rank) ||
        next.acknowledged_rank > 5 || !number(size) || size > 20000) return false;
    for (uint32_t i = 0; i < size; ++i) {
      uint32_t period, id, count;
      if (!number(period) || !number(id) || !number(count)) return false;
      const auto* d = Find(id);
      if (!d || (d->weekly ? (!period || period > next.week) : period != 0) ||
          count > d->targets[2] || !next.progress.emplace(std::make_pair(period, id), count).second)
        return false;
    }
    if (!(in >> token) || token != "END" || (in >> token) || next.acknowledged_rank > next.Rank()) return false;
    *this = std::move(next);
    return true;
  }
  bool Load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), {});
    return Decode(text);
  }
  bool Save(const std::filesystem::path& path) const {
    return kernel::xam::LgWriteFileAtomic(path, Encode());
  }
};
struct Display {
  uint32_t tiers = 0, completed = 0, progress = 0, target = 0, earned = 0, maximum = 0;
};
inline Display Describe(const Definition& d, uint32_t count) {
  Display out;
  out.tiers = TierCount(d);
  out.earned = Earned(d, count);
  out.maximum = Earned(d, d.targets[2]);
  while (out.completed < out.tiers && count >= d.targets[out.completed]) ++out.completed;
  const uint32_t previous = out.completed ? d.targets[out.completed - 1] : 0;
  out.progress = count - previous;
  if (out.completed < out.tiers) out.target = d.targets[out.completed] - previous;
  return out;
}
}  // namespace cc2::fame
