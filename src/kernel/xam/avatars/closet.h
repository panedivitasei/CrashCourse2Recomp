/**
 ******************************************************************************
 * ReXGlue avatar closet: imported Xbox LIVE marketplace avatar items.       *
 ******************************************************************************
 *
 * A closet directory holds one <product-guid>.bin (raw YTGR/STRB blob, the
 * marketplace item's asset_v2.bin) per item plus closet_index.tsv
 * (guid TAB categories-hex TAB bodies TAB name), produced by
 * `avatarextract --closet-import`. Items are enumerated into the Avatar
 * Editor's selection grids alongside the stock asset pack and resolved by
 * their full product GUID (stock pack ids resolve by pack index and always
 * carry the C1C8F109A19CB2E0 tail; marketplace ids end in the item's title
 * id, so the two populations cannot collide).
 */

#ifndef REX_AVATARS_CLOSET_H_
#define REX_AVATARS_CLOSET_H_

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "asset_pack.h"

namespace rex {
namespace avatars {

struct ClosetItem {
  AssetId id;
  uint32_t categories;
  uint8_t bodies;
  std::string name;  // UTF-8 display name for the selection grids
};

class Closet {
 public:
  // Loads closet_index.tsv from the directory. Missing dir/index = empty
  // closet (not an error). Safe to call repeatedly; loads once.
  bool Load(const std::filesystem::path& dir);

  bool is_loaded() const { return is_loaded_; }
  const std::vector<ClosetItem>& items() const { return items_; }

  const ClosetItem* Find(const AssetId& id) const;

  // Reads the item's raw YTGR/STRB blob from disk.
  bool ReadItemBytes(const AssetId& id, std::vector<uint8_t>& out) const;

 private:
  bool is_loaded_ = false;
  std::filesystem::path dir_;
  std::vector<ClosetItem> items_;
  std::unordered_map<std::string, size_t> by_guid_;  // guid string -> index
};

// Global closet instance, loaded next to the avatar asset pack.
Closet& GetCloset();

// True when the id carries the stock asset pack's GUID tail (resolvable by
// pack index); false for marketplace/closet product ids.
bool IsStockPackId(const AssetId& id);

}  // namespace avatars
}  // namespace rex

#endif  // REX_AVATARS_CLOSET_H_
