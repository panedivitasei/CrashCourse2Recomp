/**
 ******************************************************************************
 * ReXGlue avatar closet: imported Xbox LIVE marketplace avatar items.       *
 ******************************************************************************
 */

#include "closet.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/logging.h>

#include "cc2/avatar_fetch.h"

#include "xe_compat.h"

namespace rex {
namespace avatars {

static const uint8_t kStockGuidTail[8] = {0xC1, 0xC8, 0xF1, 0x09,
                                          0xA1, 0x9C, 0xB2, 0xE0};

bool IsStockPackId(const AssetId& id) {
  return std::memcmp(id.d, kStockGuidTail, sizeof(kStockGuidTail)) == 0;
}

// "aaaaaaaa-bbbb-cccc-dddd-dddddddddddd" -> AssetId. Returns false on any
// malformed field.
static bool ParseGuid(const std::string& s, AssetId* out) {
  if (s.size() != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') {
    return false;
  }
  auto hex = [](const std::string& part, uint64_t* value) {
    char* end = nullptr;
    *value = std::strtoull(part.c_str(), &end, 16);
    return end && *end == '\0';
  };
  uint64_t a, b, c, d0, d1;
  if (!hex(s.substr(0, 8), &a) || !hex(s.substr(9, 4), &b) || !hex(s.substr(14, 4), &c) ||
      !hex(s.substr(19, 4), &d0) || !hex(s.substr(24, 12), &d1)) {
    return false;
  }
  *out = AssetId{};
  out->a = static_cast<uint32_t>(a);
  out->b = static_cast<uint16_t>(b);
  out->c = static_cast<uint16_t>(c);
  out->d[0] = static_cast<uint8_t>(d0 >> 8);
  out->d[1] = static_cast<uint8_t>(d0 >> 0);
  for (int i = 0; i < 6; ++i) {
    out->d[2 + i] = static_cast<uint8_t>(d1 >> (8 * (5 - i)));
  }
  return true;
}

bool Closet::Load(const std::filesystem::path& dir) {
  if (is_loaded_) {
    return true;
  }
  dir_ = dir;
  const std::filesystem::path index_path = dir / "closet_index.tsv";
  FILE* f = std::fopen(index_path.string().c_str(), "rb");
  if (!f) {
    return false;  // no closet: fine
  }
  std::fseek(f, 0, SEEK_END);
  long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::string text((size_t)std::max(0L, sz), '\0');
  size_t got = std::fread(text.data(), 1, text.size(), f);
  std::fclose(f);
  text.resize(got);

  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    // guid \t categories-hex \t bodies \t name
    size_t t1 = line.find('\t');
    size_t t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
    size_t t3 = t2 == std::string::npos ? std::string::npos : line.find('\t', t2 + 1);
    if (t3 == std::string::npos) continue;
    ClosetItem item{};
    const std::string guid = line.substr(0, t1);
    if (!ParseGuid(guid, &item.id)) continue;
    item.categories = (uint32_t)std::strtoul(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 16);
    item.bodies = (uint8_t)std::strtoul(line.substr(t2 + 1, t3 - t2 - 1).c_str(), nullptr, 10);
    item.name = line.substr(t3 + 1);
    items_.push_back(std::move(item));
  }
  // Name order for the selection grids (the index file is guid-ordered).
  std::sort(items_.begin(), items_.end(),
            [](const ClosetItem& a, const ClosetItem& b) { return a.name < b.name; });
  for (size_t i = 0; i < items_.size(); ++i) {
    by_guid_[items_[i].id.to_string()] = i;
  }
  is_loaded_ = true;
  REXKRNL_INFO("[avatar] closet loaded: {} items from {}", items_.size(), dir.string());
  return true;
}

const ClosetItem* Closet::Find(const AssetId& id) const {
  if (items_.empty() || IsStockPackId(id)) {
    return nullptr;
  }
  auto it = by_guid_.find(id.to_string());
  return it == by_guid_.end() ? nullptr : &items_[it->second];
}

bool Closet::ReadItemBytes(const AssetId& id, std::vector<uint8_t>& out) const {
  const ClosetItem* item = Find(id);
  if (!item) {
    // A lobby peer's item this closet lacks may sit in the render cache; it stays unlisted and unowned.
    return !IsStockPackId(id) && cc2::avatar_fetch::ReadCachedItem(id.to_string(), out);
  }
  const std::filesystem::path path = dir_ / (id.to_string() + ".bin");
  FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) {
    REXKRNL_WARN("[avatar] closet item file missing: {}", path.string());
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.resize((size_t)std::max(0L, sz));
  size_t got = std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  return sz > 0 && got == out.size();
}

Closet& GetCloset() {
  static Closet closet;
  return closet;
}

}  // namespace avatars
}  // namespace rex
