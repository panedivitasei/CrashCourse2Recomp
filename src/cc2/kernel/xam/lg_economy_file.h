/**
 * @file        lg_economy_file.h
 * @brief       Persistent state of the emulated LiveGamer (Loyalty Gateway)
 *              economy: currency balances + owned item unlocks, stored as a
 *              local XML file in <user_data_root>/economy.xml.
 *
 * The local HTTP service commits balances and ownership here; the native
 * LiveGamer handlers load the resulting account and inventory documents.
 *
 * Format:
 *   <?xml version="1.0" encoding="utf-8"?>
 *   <economy>
 *     <balances stars="5" totalStars="5" coins="0" rewinds="0"/>
 *     <owned>
 *       <item id="1001"/>
 *     </owned>
 *   </economy>
 *
 * "owned" ids are LiveGamer item ids — for DCC2 these are the levelconfig
 * LgUnlockId / LgSplitPathId values the title itself matches unlocks against.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License — see LICENSE in the project root.
 */
#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>

namespace cc2 {
namespace kernel {
namespace xam {

// Consumables as LiveGamer forex offers (CCSTARS -> one unit); currency names must match the title's
// templates (0x822984E8 against MODIFIERMANAGER rec+116 / JINXMANAGER rec+112).
// Offer and account ids are ours but must stay stable: the game stamps account ids into rec+120+16*profileIdx.
struct LgConsumableDef {
  const char* type;      // shop category: "rewind" | "powerup" | "jinx"
  const char* currency;  // toCurrency name (title-matched)
  uint32_t offer_id;     // forex offer id (server-chosen, stable)
  uint32_t account_id;   // player's per-currency account id (server-chosen)
  uint32_t price_stars;  // CCSTARS per purchase
  uint32_t qty;          // units credited per purchase
};
inline constexpr LgConsumableDef kLgConsumables[] = {
    {"rewind", "CCREWINDS", 3201, 104, 5, 3},  // rewinds pack (account = kAccRewinds)
    {"powerup", "CCPSLIPSLIDE", 3001, 201, 10, 1},
    {"powerup", "CCPSUPERSTRONG", 3002, 202, 10, 1},
    {"powerup", "CCPSUPERBOOST", 3003, 203, 10, 1},
    {"powerup", "CCPWALLBOOST", 3004, 204, 10, 1},
    {"powerup", "CCPSTATICCLING", 3005, 205, 10, 1},
    {"jinx", "CCJPRINCESS", 3101, 211, 8, 1},
    {"jinx", "CCJUNICORN", 3102, 212, 8, 1},
    {"jinx", "CCJRAINBOW", 3103, 213, 8, 1},
    {"jinx", "CCJAQUA", 3104, 214, 8, 1},
    {"jinx", "CCJCHICKEN", 3105, 215, 8, 1},
    {"jinx", "CCJSHARK", 3106, 216, 8, 1},
    {"jinx", "CCJSICKDOG", 3107, 217, 8, 1},
    {"jinx", "CCJBABY", 3108, 218, 8, 1},
    {"jinx", "CCJTOILET", 3109, 219, 8, 1},
    {"jinx", "CCJHUMPTY", 3110, 220, 8, 1},
};

// Vanities are LG store items, and 0x822C9478 drops any whose id doesn't match a VANITYMANAGER ItemId (rec+104).
// Ids come from vanity.wbr; offer id == item id, and ownership is the byte at rec+108+profileIdx (0 = owned).
struct LgVanityDef {
  const char* name;  // vanity.wbr record key (diagnostic only)
  uint32_t item_id;  // LiveGamer ItemId (vanity.wbr; served as both item and offer id)
};
inline constexpr LgVanityDef kLgVanities[] = {
    {"comet1", 1071},    {"rainbow1", 1072},  {"dirty1", 1073},
    {"fire1", 1074},     {"stormgod1", 1075}, {"musicman1", 1076},
};

inline bool LgWriteFileAtomic(const std::filesystem::path& path, std::string_view xml) {
    const std::filesystem::path tmp = path.wstring() + L".tmp";
#ifdef _WIN32
    const HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool saved = WriteFile(file, xml.data(), static_cast<DWORD>(xml.size()),
                                 &written, nullptr) && written == xml.size() &&
                       FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != 0;
    if (!saved || !closed) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(xml.data(), static_cast<std::streamsize>(xml.size()));
    f.flush();
    if (!f) return false;
    f.close();
    if (!f) return false;
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
#endif
}

struct LgEconomyFile {
  uint32_t stars = 0;
  uint32_t total_stars = 0;
  uint32_t coins = 0;
  uint32_t rewinds = 0;
  std::set<uint32_t> owned;  // LiveGamer item ids (LgUnlockId / LgSplitPathId)
  // Consumable inventory: currency name (CCP*/CCJ*) -> count. Rewinds stay in
  // the `rewinds` balance above (they are the CCREWINDS account).
  std::map<std::string, uint32_t> consumables;

  // Attribute lookup is confined to one start tag; malformed numbers never become balances.
  static std::optional<std::string_view> Attr(std::string_view xml, size_t from,
                                              std::string_view name) {
    if (from >= xml.size()) return std::nullopt;
    const auto end = xml.find('>', from);
    if (end == std::string_view::npos) return std::nullopt;
    const auto tag = xml.substr(from, end - from);
    const std::string key = std::string(name) + "=\"";
    size_t at = 0;
    while ((at = tag.find(key, at)) != std::string_view::npos) {
      if (at && (tag[at - 1] == ' ' || tag[at - 1] == '\t' ||
                 tag[at - 1] == '\r' || tag[at - 1] == '\n')) {
        const auto start = at + key.size();
        const auto stop = tag.find('"', start);
        if (stop != std::string_view::npos) return tag.substr(start, stop - start);
        return std::nullopt;
      }
      at += key.size();
    }
    return std::nullopt;
  }

  static bool ReadUInt(std::string_view value, uint32_t& out) {
    if (value.empty()) return false;
    uint32_t n = 0;
    for (const char c : value) {
      if (c < '0' || c > '9' ||
          n > (std::numeric_limits<uint32_t>::max() - (c - '0')) / 10u) return false;
      n = n * 10u + (c - '0');
    }
    out = n;
    return true;
  }

  static bool ReadAttr(std::string_view xml, size_t from, std::string_view name,
                       uint32_t& out) {
    const auto value = Attr(xml, from, name);
    return value && ReadUInt(*value, out);
  }

  static uint32_t ParseAttr(const std::string& xml, size_t from, const char* name,
                            uint32_t fallback) {
    uint32_t value;
    return ReadAttr(xml, from, name, value) ? value : fallback;
  }

  static std::string ParseStrAttr(const std::string& xml, size_t from, const char* name) {
    const auto value = Attr(xml, from, name);
    return value ? std::string(*value) : std::string();
  }

  bool Load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
      return false;
    }
    std::string xml((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t bal = xml.find("<balances");
    if (bal == std::string::npos) {
      return false;
    }
    LgEconomyFile next;
    if (xml.find("</economy>") == std::string::npos ||
        !ReadAttr(xml, bal, "stars", next.stars) ||
        !ReadAttr(xml, bal, "totalStars", next.total_stars) ||
        !ReadAttr(xml, bal, "coins", next.coins) ||
        !ReadAttr(xml, bal, "rewinds", next.rewinds)) return false;
    for (size_t at = xml.find("<item "); at != std::string::npos;
         at = xml.find("<item ", at + 1)) {
      uint32_t id;
      if (!ReadAttr(xml, at, "id", id) || !id) return false;
      next.owned.insert(id);
    }
    for (size_t at = xml.find("<consumable "); at != std::string::npos;
         at = xml.find("<consumable ", at + 1)) {
      const std::string currency = ParseStrAttr(xml, at, "currency");
      uint32_t count;
      if (currency.empty() || !ReadAttr(xml, at, "count", count)) return false;
      next.consumables[currency] = count;
    }
    *this = std::move(next);
    return true;
  }

  bool Save(const std::filesystem::path& path) const {
    std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<economy>\r\n";
    xml += "  <balances stars=\"" + std::to_string(stars) + "\" totalStars=\"" +
           std::to_string(total_stars) + "\" coins=\"" + std::to_string(coins) +
           "\" rewinds=\"" + std::to_string(rewinds) + "\"/>\r\n";
    xml += "  <owned>\r\n";
    for (const uint32_t id : owned) {
      xml += "    <item id=\"" + std::to_string(id) + "\"/>\r\n";
    }
    xml += "  </owned>\r\n";
    xml += "  <consumables>\r\n";
    for (const auto& [currency, count] : consumables) {
      xml += "    <consumable currency=\"" + currency + "\" count=\"" + std::to_string(count) +
             "\"/>\r\n";
    }
    xml += "  </consumables>\r\n</economy>\r\n";
    return LgWriteFileAtomic(path, xml);
  }
};


// Persist an item awarded by the native RewardMASTER eligibility flow.
bool LgEconomyGrantItem(uint32_t item_id);

}  // namespace xam
}  // namespace kernel
}  // namespace cc2
