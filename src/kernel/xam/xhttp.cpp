/**
 * @file        xhttp.cpp
 * @brief       Online client API surface: NetDll_XHttp* (Xbox 360 HTTP client,
 *              WinHttp-alike) and the XampXAuth* authentication entry points.
 *
 * Implements the request lifecycle (Startup → Open → Connect → OpenRequest →
 * SendRequest → ReceiveResponse → Read/QueryHeaders → CloseHandle) with opaque
 * handles. There is no network transport: ServeRequest answers every request
 * in-process and completions reach the title through its async status
 * callback, pumped by XHttpDoWork.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License — see LICENSE in the project root.
 */

#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include "cc2/kernel/xam/lg_economy_file.h"
#include "cc2/kernel/xam/lg_transactions.h"
#include "cc2/kernel/xnet.h"
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

// game_data_root is defined by the runtime (runtime.cpp) at global scope. It
// holds lg_dlc_levels.xml and the legacy economy.xml location.
REXCVAR_DECLARE(std::string, game_data_root);

namespace rex {
namespace kernel {
namespace xam {

using namespace cc2::kernel;
using namespace cc2::kernel::xam;

// Opaque XHTTP handles (HINTERNET-alike). Request handles key into g_requests,
// which holds the state ServeRequest answers from.
static std::atomic<uint32_t> g_xhttp_next_handle{0x58A00001u};

static inline uint32_t alloc_xhttp_handle() {
  return g_xhttp_next_handle.fetch_add(4, std::memory_order_relaxed);
}

// WinHttp/XHttp query info levels (subset).
static constexpr uint32_t kQueryStatusCode = 19;
static constexpr uint32_t kQueryFlagNumber = 0x20000000u;
static constexpr uint32_t kQueryLevelMask = 0x1FFFFFFFu;

// WINHTTP_CALLBACK_STATUS_* notification codes (desktop WinHttp values; XHTTP
// mirrors them). The title opens the session with WINHTTP_FLAG_ASYNC
// (0x10000000) and registers a status callback, so completions are delivered
// through that callback from XHttpDoWork, not synchronously.
static constexpr uint32_t kCbHeadersAvailable = 0x00020000u;
static constexpr uint32_t kCbDataAvailable = 0x00040000u;
static constexpr uint32_t kCbReadComplete = 0x00080000u;
static constexpr uint32_t kCbSendRequestComplete = 0x00400000u;

struct XHttpRequest {
  std::string verb;
  std::string path;
  std::string upload_body;   // PUT/POST request body (captured from XHttpSendRequest)
  int status = 0;            // HTTP status the emulator produced
  std::string body;          // response body
  size_t read_offset = 0;    // XHttpReadData cursor
  uint32_t callback = 0;     // guest WINHTTP_STATUS_CALLBACK address
  uint32_t context = 0;      // dwContext passed to the callback
  uint32_t scratch = 0;      // guest DWORD scratch for DATA_AVAILABLE counts
};

// A pending async completion to deliver to a request's status callback.
struct XHttpNotification {
  uint32_t request_handle = 0;
  uint32_t status = 0;     // WINHTTP_CALLBACK_STATUS_*
  uint32_t info_ptr = 0;   // guest lpvStatusInformation
  uint32_t info_len = 0;   // dwStatusInformationLength
  uint32_t info_value = 0; // for DATA_AVAILABLE: byte count written to scratch
  uint64_t ready_at_ms = 0;  // earliest steady-clock delivery time (0 = now)
};

// Emulated network round-trip. Without it a request completes on the next
// DoWork pump, inside the scheduler slice that issued it. The shop/marketplace
// task graph assumes real latency: if a child task finishes before its parent
// registers it, the parent tears down early and OnChildStopped iterates a
// freed std::map (guest null reads as zeros, so node 0 loops forever with one
// core pegged). Delaying only SEND_REQUEST_COMPLETE models one RTT per request
// (headers/reads then flow at pump speed) and returns the scheduler slice to
// the parent before any child can complete.
REXCVAR_DEFINE_INT32(xhttp_response_delay_ms, 35, "Kernel",
                     "Minimum emulated network latency per request (ms). "
                     "0 = answer on the next pump (hangs the shop task graph).");

static uint64_t XHttpNowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

static std::mutex g_xhttp_mu;
static std::unordered_map<uint32_t, XHttpRequest> g_requests;  // request handle -> state
static std::vector<XHttpNotification> g_notifications;          // pending async completions

// cert/config.xml is the LiveGamer (Loyalty Gateway) bootstrap document.
// LiveGamer_FetchConfigXml_Start (0x82298B68) GETs it and on HTTP 200 passes the
// body to KERNELCOMMANDSXA__ApplyLgInfoJson (0x82B013B8), which parses it with a
// rapidxml DOM parser (0x82246CE0; errors "expected <" on non-XML, so JSON is
// rejected). Values are element text (child looked up by name via 0x822430B0;
// value at node+4). Required children; a missing one takes the failure path to
// Shop::Reset and the level-select / shop / unlock chain stays dead:
//   lgServer, lgAuthEndpoint, progressEndpoint, lgStore{id,name}.
// LG request URLs are built as "<lgServer>/<lgAuthEndpoint>/store/items?..." so
// lgAuthEndpoint is a path segment, not a host. Interception is path-based (the
// host is never contacted): every endpoint points at the virtual host and
// ServeRequest routes on path prefixes. The Sentient "Config Rest" resource
// (<RestConfigMASTER/>) is local to scripts.wbr/restconfig.wbr, not this file.
//
// lgStore/featured: up to ten child elements, each a "<category>/<item>" path the Featured tab resolves
// (sub_82B00248: root Rewinds/Powerups/Jinxes/Vanity/Courses, then the item's record key in that category).
constexpr const char* kLgFeaturedPool[] = {
    "Powerups/slip_slide", "Powerups/super_strong", "Powerups/super_boost", "Powerups/wall_boost",
    "Powerups/static_cling", "Jinxes/hada",          "Jinxes/unicorn",       "Jinxes/nam_nam",
    "Jinxes/scuba",          "Jinxes/chicken",       "Jinxes/Shark",         "Jinxes/sick_dog",
    "Jinxes/baby",           "Jinxes/wc_head",       "Jinxes/humpty",        "Vanity/comet",
    "Vanity/musicman",       "Vanity/dirty",         "Vanity/stormgod",      "Vanity/rainbow",
    "Vanity/fire",
};
constexpr size_t kLgFeaturedCount = 6;

// A fresh featured set each week (Monday 00:00 UTC), the same for every launch within it.
static std::string BuildLgFeaturedXml() {
  const int64_t days = std::chrono::duration_cast<std::chrono::hours>(
                           std::chrono::system_clock::now().time_since_epoch()).count() / 24;
  std::vector<const char*> pool(std::begin(kLgFeaturedPool), std::end(kLgFeaturedPool));
  std::mt19937 rng(static_cast<uint32_t>((days + 3) / 7));
  for (size_t i = pool.size() - 1; i > 0; --i) {
    std::swap(pool[i], pool[std::uniform_int_distribution<size_t>(0, i)(rng)]);
  }
  std::string xml = "<featured>";
  for (size_t i = 0; i < kLgFeaturedCount; ++i) xml += std::string("<item>") + pool[i] + "</item>";
  return xml + "</featured>";
}

static std::string BuildLgConfigXml() {
  return
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
      "<lgInfo>"
      "<isProduction>true</isProduction>"
      "<achievementsLocked>false</achievementsLocked>"
      "<goldWallActive>false</goldWallActive>"
      "<invitationsEnabled>false</invitationsEnabled>"
      "<coinPurchasesEnabled>false</coinPurchasesEnabled>"
      "<giftCoins>0</giftCoins>"
      "<showBetaLabel>false</showBetaLabel>"
      "<lgServer>http://dcc2.service.rexglue/livegamer</lgServer>"
      "<lgAuthEndpoint>lg</lgAuthEndpoint>"
      "<progressEndpoint>http://dcc2.service.rexglue/progress</progressEndpoint>"
      "<lgStore>"
      "<id>1480659535</id>"
      "<name>CrashCourse2</name>" +
      BuildLgFeaturedXml() +
      "<new/>"
      "<sale/>"
      "<coming_soon/>"
      "</lgStore>"
      "</lgInfo>\r\n";
}

// LiveGamer store catalog: the per-level cost/unlock source for the native
// course-select. LiveGamer_ParseLevelItems_BuildTree (0x822CA1F0) issues one
// "store/items?...&p.theme=<theme>&locale=en_US" GET per theme; the response is
// parsed by TaskSystem::LiveGamer::ParseLevelItems::Start (sub_82297A50):
// store/items/item/offers/offer/price/money{amount,currencyName} with
// currencyName in {CCSTARS, CCCOINS}, plus item properties key=type/id.
// Property type=="level" feeds the main cost, any other type the split-path
// cost; the level is matched by the "id" property (sub_8223C360(mgr+60)).
// The table is extracted from the title's data/templates/levelconfig.wbr
// (LEVELCONFIGMASTER, 28 records): LevelId, SavingID, LgUnlockId,
// LgSplitPathId and the default coin/star costs. All 28 levels are served on
// every store/items request regardless of p.theme: the parser matches items to
// levels by id, so off-theme items are either matched with identical data or
// logged and skipped.
struct LgLevel {
  const char* key;      // LEVELCONFIGMASTER record key (diagnostic only)
  uint32_t level_id;    // LevelId field (served as the "id" property)
  uint32_t saving_id;   // SavingID field (unused)
  uint32_t lg_unlock;   // LgUnlockId (main-unlock offer id)
  uint32_t lg_split;    // LgSplitPathId (split-path offer id)
  uint32_t coins;       // CoinsCost
  uint32_t stars;       // StarsCost
  uint32_t sp_coins;    // SplitPathCoinsCost
  uint32_t sp_stars;    // SplitPathStarsCost
};
static constexpr LgLevel kLgLevels[] = {
    {"dlc_jungle/track_01", 1000, 0, 1001, 1036, 5, 5, 10, 10},
    {"dlc_jungle/track_02", 1100, 1, 1002, 1037, 5, 5, 10, 10},
    {"dlc_jungle/track_03", 1200, 2, 1003, 1038, 5, 5, 10, 10},
    {"dlc_jungle/track_04", 1300, 3, 1004, 1039, 5, 5, 10, 10},
    {"dlc_jungle/track_05", 1400, 4, 1005, 1040, 5, 5, 10, 10},
    {"dlc_jungle/track_bonus_01", 1500, 5, 1006, 1041, 5, 5, 10, 10},
    {"dlc_jungle/track_bonus_02", 1600, 6, 1007, 1042, 5, 5, 10, 10},
    {"dlc_arctic/track_01", 2000, 7, 1008, 1043, 10, 10, 10, 10},
    {"dlc_arctic/track_02", 2100, 8, 1009, 1044, 10, 10, 10, 10},
    {"dlc_arctic/track_03", 2200, 9, 1010, 1045, 10, 10, 10, 10},
    {"dlc_arctic/track_04", 2300, 10, 1011, 1046, 10, 10, 10, 10},
    {"dlc_arctic/track_05", 2400, 11, 1012, 1047, 10, 10, 10, 10},
    {"dlc_arctic/track_bonus_01", 2500, 12, 1013, 1048, 10, 10, 10, 10},
    {"dlc_arctic/track_bonus_02", 2600, 13, 1014, 1049, 10, 10, 10, 10},
    {"dlc_desert/track_01", 3000, 14, 1015, 1050, 10, 10, 10, 10},
    {"dlc_desert/track_02", 3100, 15, 1016, 1051, 10, 10, 10, 10},
    {"dlc_desert/track_03", 3200, 16, 1017, 1052, 10, 10, 10, 10},
    {"dlc_desert/track_04", 3300, 17, 1018, 1053, 10, 10, 10, 10},
    {"dlc_desert/track_05", 3400, 18, 1019, 1054, 10, 10, 10, 10},
    {"dlc_desert/track_bonus_01", 3500, 19, 1020, 1055, 10, 10, 10, 10},
    {"dlc_desert/track_bonus_02", 3600, 20, 1021, 1056, 10, 10, 10, 10},
    {"dlc_island/track_01", 4000, 21, 1022, 1057, 10, 10, 10, 10},
    {"dlc_island/track_02", 4100, 22, 1023, 1058, 10, 10, 10, 10},
    {"dlc_island/track_03", 4200, 23, 1024, 1059, 10, 10, 10, 10},
    {"dlc_island/track_04", 4300, 24, 1025, 1060, 10, 10, 10, 10},
    {"dlc_island/track_05", 4400, 25, 1026, 1061, 10, 10, 10, 10},
    {"dlc_island/track_bonus_01", 4500, 26, 1027, 1062, 10, 10, 10, 10},
    {"dlc_island/track_bonus_02", 4600, 27, 1028, 1063, 10, 10, 10, 10},
};

// Uniform star price for every level/split-path unlock. Overrides the
// per-level levelconfig costs in both the served catalog (what the
// course-select displays) and the purchase charge, keeping them consistent.
// 0 = use the per-level table values.
REXCVAR_DEFINE_INT32(dcc2_lg_star_price, 0, "Kernel",
                     "Uniform star price for every LiveGamer level/split-path unlock "
                     "(catalog display + purchase charge). 0 = per-level table values.")
    .range(0, 100000);

static uint32_t LgStarsFor(uint32_t table_value) {
  const auto uniform = static_cast<uint32_t>(REXCVAR_GET(dcc2_lg_star_price));
  return uniform ? uniform : table_value;
}

// Levelconfig export from the game side (<game_data_root>/lg_dlc_levels.xml,
// written by src/sentient.cpp): the level table loaded this boot, base plus
// expansion packs (Hong Kong). Shop/course-select rows take their offer id
// from this catalog, not from the levelconfig record: a level missing from
// store/items POSTs itemOfferRequest id=0 and can never be purchased or
// persisted. Only entries not covered by the static kLgLevels table are kept.
// Reloaded on mtime change.
struct LgDlcLevel {
  uint32_t level_id;   // LevelId (the store item "id" property match key)
  uint32_t unlock_id;  // LgUnlockId (real from the pak, or synthetic 20000+)
  uint32_t split_id;   // LgSplitPathId
  uint32_t coins, stars, split_coins, split_stars;
};
static std::mutex g_dlc_mu;

static std::filesystem::path DlcLevelsPath() {
  const std::string root = REXCVAR_GET(game_data_root);
  return root.empty() ? std::filesystem::path("lg_dlc_levels.xml")
                      : std::filesystem::path(root) / "lg_dlc_levels.xml";
}

static std::vector<LgDlcLevel> DlcLevels() {
  static std::vector<LgDlcLevel> levels;
  static std::filesystem::file_time_type last_mtime{};
  std::lock_guard<std::mutex> lock(g_dlc_mu);
  std::error_code ec;
  const auto mtime = std::filesystem::last_write_time(DlcLevelsPath(), ec);
  if (!ec && mtime != last_mtime) {
    last_mtime = mtime;
    levels.clear();
    std::ifstream f(DlcLevelsPath(), std::ios::binary);
    if (f) {
      std::string xml((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      for (size_t at = xml.find("<level "); at != std::string::npos;
           at = xml.find("<level ", at + 1)) {
        const LgDlcLevel lv{LgEconomyFile::ParseAttr(xml, at, "id", 0),
                            LgEconomyFile::ParseAttr(xml, at, "unlock", 0),
                            LgEconomyFile::ParseAttr(xml, at, "split", 0),
                            LgEconomyFile::ParseAttr(xml, at, "coins", 10),
                            LgEconomyFile::ParseAttr(xml, at, "stars", 10),
                            LgEconomyFile::ParseAttr(xml, at, "splitCoins", 10),
                            LgEconomyFile::ParseAttr(xml, at, "splitStars", 10)};
        if (!lv.unlock_id) {
          continue;
        }
        bool known = false;
        for (const auto& base : kLgLevels) {
          if (base.lg_unlock == lv.unlock_id ||
              (lv.level_id && base.level_id == lv.level_id)) {
            known = true;
            break;
          }
        }
        if (!known) {
          levels.push_back(lv);
        }
      }
      REXKRNL_DEBUG("[lgecon] loaded {} DLC level(s) beyond the base catalog from {}",
                   levels.size(), DlcLevelsPath().string());
    }
  }
  return levels;
}

// Star price of a vanity ("Avatar Effects") item. The original store prices are
// unavailable; a flat configurable price keeps the buy-to-unlock loop.
REXCVAR_DEFINE_INT32(dcc2_lg_vanity_price, 25, "Kernel",
                     "Star price of every LiveGamer vanity (shop Avatar Effects tab) item "
                     "(catalog display + purchase charge).")
    .range(0, 100000);

// The Avatar Effects tab catalog. Parser (Shop::PopulateVanityItemCategoryTask
// ::OnChildStopped 0x822C9478) expects the same <response><store><items> root
// as the level catalog but reads the item id attribute; level items carry the
// id as a <property>, so the level document yields id 0 for every entry
// ("Couldn't find vanity item with Live Gamer id 0"). Each item id must match a
// VANITYMANAGER record's LG ItemId (rec+104, from vanity.wbr); each offer needs
// price>money{amount,currencyName in CCSTARS|CCCOINS}. Offer id == item id so
// the purchase POST's itemOfferRequest@id identifies the vanity to own.
static std::string BuildLgVanityItemsXml() {
  const uint32_t price = static_cast<uint32_t>(REXCVAR_GET(dcc2_lg_vanity_price));
  std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<response><store><items>";
  for (const auto& v : kLgVanities) {
    xml += "<item id=\"" + std::to_string(v.item_id) + "\"><offers>";
    xml += "<offer id=\"" + std::to_string(v.item_id) + "\"><price><money amount=\"" +
           std::to_string(price) + "\" currencyName=\"CCSTARS\"/></price></offer>";
    xml += "</offers></item>";
  }
  xml += "</items></store></response>\r\n";
  return xml;
}

static std::string BuildLgStoreItemsXml() {
  std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<response><store><items>";
  auto item = [&](uint32_t offer_id, uint32_t coins, uint32_t stars, const char* type,
                  uint32_t match_id) {
    xml += "<item><offers>";
    xml += "<offer id=\"" + std::to_string(offer_id) + "\"><price><money amount=\"" +
           std::to_string(stars) + "\" currencyName=\"CCSTARS\"/></price></offer>";
    xml += "<offer id=\"" + std::to_string(offer_id) + "\"><price><money amount=\"" +
           std::to_string(coins) + "\" currencyName=\"CCCOINS\"/></price></offer>";
    xml += "</offers><properties>";
    xml += std::string("<property key=\"type\" value=\"") + type + "\"/>";
    xml += "<property key=\"id\" value=\"" + std::to_string(match_id) + "\"/>";
    xml += "</properties></item>";
  };
  for (const auto& lv : kLgLevels) {
    item(lv.lg_unlock, lv.coins, LgStarsFor(lv.stars), "level", lv.level_id);
    item(lv.lg_split, lv.sp_coins, LgStarsFor(lv.sp_stars), "splitPath", lv.level_id);
  }
  // Runtime-discovered DLC levels (Hong Kong): serve offers so their rows
  // carry a real offer id -- a level with no catalog item POSTs
  // itemOfferRequest id=0 and the purchase can never be recorded.
  for (const auto& lv : DlcLevels()) {
    // Matched by LevelId; the export self-calibrates the field.
    if (!lv.level_id) {
      continue;  // calibration failed; the stamped record costs still show
    }
    item(lv.unlock_id, lv.coins, LgStarsFor(lv.stars), "level", lv.level_id);
    item(lv.split_id, lv.split_coins, LgStarsFor(lv.split_stars), "splitPath", lv.level_id);
  }
  xml += "</items></store></response>\r\n";
  return xml;
}

//=============================================================================
// LiveGamer economy: server-side balances and owned unlocks, persisted to
// <user_data_root>/economy.xml (LgEconomyFile). Title design: the player
// starts with a small star balance, buys the first level, earns stars
// in-level (currencyTransfers credits) and spends them on further unlocks
// (purchaseOrder debits). The title syncs its profile counters from the
// served "user" document (KERNELCOMMANDSXA parse
// 0x822984E8: CCSTARS -> profile+5672, CCTOTALSTARS -> +5676, CCCOINS ->
// +5680, CCREWINDS -> +5684) and its per-level unlock bits from the
// "itemInstances" document (parse 0x822F4AD8 -> 0x82B33BD8 matches each
// itemInstance@itemId against levelconfig LgUnlockId/LgSplitPathId and clears
// the lock byte at levelObj+profileIdx+304/308).
//=============================================================================

REXCVAR_DEFINE_INT32(dcc2_lg_start_stars, 10, "Kernel",
                     "Star balance a fresh LiveGamer economy file starts with (the intended "
                     "design: exactly enough to buy the first level -- keep in step with "
                     "dcc2_lg_star_price).")
    .range(0, 100000);

// Account ids we issue in the "user" document. The title stores them from the
// account parse and uses them in later currencyTransfers debit/credit pairs.
static constexpr uint32_t kAccStars = 101;
static constexpr uint32_t kAccTotalStars = 102;
static constexpr uint32_t kAccCoins = 103;
static constexpr uint32_t kAccRewinds = 104;
static constexpr uint32_t kLgUserId = 1000;

static std::mutex g_econ_mu;
static LgEconomyFile g_econ;
static bool g_econ_loaded = false;
static bool g_econ_ready = false;

// Economy (stars/coins/unlocks) is save data: it lives in user_data_root so it
// travels and resets with the portable userdata folder. MigrateLegacyEconomy
// moves an economy.xml found in game_data_root (the legacy location) there
// once, so wiping userdata is a complete reset.
static std::filesystem::path EconomyPath() {
  auto* ks = REX_KERNEL_STATE();
  if (ks && ks->emulator() && !ks->emulator()->user_data_root().empty()) {
    return ks->emulator()->user_data_root() / "economy.xml";
  }
  return std::filesystem::path("economy.xml");
}

static void MigrateLegacyEconomy() {
  const std::string root = REXCVAR_GET(game_data_root);
  const std::filesystem::path legacy =
      root.empty() ? std::filesystem::path("economy.xml")
                   : std::filesystem::path(root) / "economy.xml";
  const std::filesystem::path dest = EconomyPath();
  if (legacy == dest) {
    return;
  }
  std::error_code ec;
  if (!std::filesystem::exists(legacy, ec) || std::filesystem::exists(dest, ec)) {
    return;
  }
  std::filesystem::create_directories(dest.parent_path(), ec);
  std::filesystem::rename(legacy, dest, ec);
  if (ec) {
    ec.clear();
    std::filesystem::copy_file(legacy, dest, ec);
    if (!ec) {
      std::filesystem::remove(legacy, ec);
    }
  }
  if (!ec) {
    REXKRNL_INFO("[lgecon] migrated economy.xml from game_data_root to {}", dest.string());
  }
}

// Call with g_econ_mu held.
static LgEconomyFile& Economy() {
  if (!g_econ_loaded) {
    g_econ_loaded = true;
    MigrateLegacyEconomy();
    std::error_code ec;
    const auto path = EconomyPath();
    if (g_econ.Load(path)) {
      g_econ_ready = true;
    } else if (!std::filesystem::exists(path, ec) && !ec) {
      g_econ.stars = static_cast<uint32_t>(REXCVAR_GET(dcc2_lg_start_stars));
      g_econ.total_stars = g_econ.stars;
      std::filesystem::create_directories(path.parent_path(), ec);
      g_econ_ready = !ec && g_econ.Save(path);
    }
    if (g_econ_ready) {
      REXKRNL_INFO("[lgecon] loaded {} (stars={} total={} coins={} rewinds={} owned={})",
                   path.string(), g_econ.stars, g_econ.total_stars, g_econ.coins,
                   g_econ.rewinds, g_econ.owned.size());
    } else {
      REXKRNL_ERROR("[lgecon] cannot load or create {}; local economy requests will fail without replacing the save",
                    path.string());
    }
  }
  return g_econ;
}

// The "user" document (LiveGamer account + currency balances). Parsed by the
// title's user-fetch task (0x822981A8 -> 0x822984E8): child <accounts> is
// iterated, each account element needs attrs id/balance and a <currency name=>
// child; <properties> children need name=/value= attrs.
static std::string BuildLgUserXml() {
  const LgEconomyFile& econ = Economy();
  auto account = [](uint32_t id, uint32_t balance, const char* currency) {
    return "<account id=\"" + std::to_string(id) + "\" balance=\"" + std::to_string(balance) +
           "\"><currency name=\"" + currency + "\"/></account>";
  };
  std::string accounts = account(kAccStars, econ.stars, "CCSTARS") +
                         account(kAccTotalStars, econ.total_stars, "CCTOTALSTARS") +
                         account(kAccCoins, econ.coins, "CCCOINS") +
                         account(kAccRewinds, econ.rewinds, "CCREWINDS");
  // Per-consumable accounts (CCP*/CCJ*): ApplyUserDoc_Balances (0x822984E8)
  // matches the currency name against the MODIFIERMANAGER/JINXMANAGER records
  // and stores {account id, balance} at rec+120+16*profileIdx.
  for (const auto& c : kLgConsumables) {
    if (std::string(c.currency) == "CCREWINDS") {
      continue;  // rewinds are the kAccRewinds balance above
    }
    const auto it = econ.consumables.find(c.currency);
    accounts += account(c.account_id, it == econ.consumables.end() ? 0u : it->second, c.currency);
  }
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
         "<user id=\"" + std::to_string(kLgUserId) + "\" externalKey=\"rexglue\">"
         "<accounts>" + accounts +
         "</accounts>"
         "<properties><property name=\"coinPurchaseCounter\" value=\"0\"/></properties>"
         "</user>\r\n";
}

// The owned-unlocks document. Parser (0x822F4AD8) walks itemInstances >
// itemInstance@itemId and unlocks the level whose levelconfig LgUnlockId or
// LgSplitPathId equals itemId (0x82B33BD8). Every owned id is served regardless
// of the query's itemTypeId filter; unmatched ids match no level.
// The first level's split path (Jungle Gym Antics) is owned from the start.
static std::string BuildLgItemInstancesXml() {
  std::set<uint32_t> owned = Economy().owned;
  owned.insert(kLgLevels[0].lg_split);
  std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<itemInstances>";
  for (const uint32_t id : owned) {
    xml += "<itemInstance itemId=\"" + std::to_string(id) + "\"/>";
  }
  xml += "</itemInstances>\r\n";
  return xml;
}

// Star price of a LiveGamer unlock offer id, from the same tables the catalog
// serves (levels/split paths + vanities). Returns false for unknown ids.
static bool LgUnlockPrice(uint32_t offer_id, uint32_t account, uint32_t& out_stars) {
  for (const auto& lv : kLgLevels) {
    if (lv.lg_unlock == offer_id) {
      out_stars = account == kAccCoins ? lv.coins : LgStarsFor(lv.stars);
      return true;
    }
    if (lv.lg_split == offer_id) {
      out_stars = account == kAccCoins ? lv.sp_coins : LgStarsFor(lv.sp_stars);
      return true;
    }
  }
  for (const auto& v : kLgVanities) {
    if (v.item_id == offer_id && account == kAccStars) {
      out_stars = static_cast<uint32_t>(REXCVAR_GET(dcc2_lg_vanity_price));
      return true;
    }
  }
  // Runtime-discovered DLC levels (lg_dlc_levels.xml) -- real pak ids or the
  // synthetic 20000+LevelId / 30000+LevelId ids the game-side sync stamps
  // onto records that ship with LgUnlockId=0. Flat-priced like everything.
  for (const auto& lv : DlcLevels()) {
    if (lv.unlock_id == offer_id) {
      out_stars = account == kAccCoins ? lv.coins : LgStarsFor(lv.stars);
      return true;
    }
    if (lv.split_id == offer_id) {
      out_stars = account == kAccCoins ? lv.split_coins : LgStarsFor(lv.split_stars);
      return true;
    }
  }
  return false;
}

// On-disk backing store for the emulated Title Storage: a blob the title PUTs
// is returned byte-for-byte by a later GET, across runs. Lives in
// "rexglue_server/" under user_data_root so per-user progress (e.g. DCC2's
// stars/medals prod.bin) travels and resets with the portable userdata folder.
// The guest path is flattened into a filename (keeping the most-specific tail
// when very long).
static std::filesystem::path BlobRoot() {
  auto* ks = REX_KERNEL_STATE();
  if (ks && ks->emulator() && !ks->emulator()->user_data_root().empty()) {
    return ks->emulator()->user_data_root() / "rexglue_server";
  }
  return std::filesystem::path("rexglue_server");
}

static std::string BlobFileName(const std::string& path) {
  std::string fn;
  fn.reserve(path.size());
  for (char c : path) {
    fn += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_';
  }
  if (fn.size() > 180) {
    fn = fn.substr(fn.size() - 180);
  }
  return fn;
}

static std::filesystem::path BlobPath(const std::string& path) {
  return BlobRoot() / BlobFileName(path);
}

// One-time migration of a legacy ./rexglue_server (working dir) into the
// userdata store. Files are moved, not read through, so wiping userdata is a
// complete reset and a leftover legacy dir cannot resurrect stale progress.
static void MigrateLegacyBlobStore() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  const std::filesystem::path legacy = "rexglue_server";
  const std::filesystem::path root = BlobRoot();
  if (root == legacy) {
    return;
  }
  std::error_code ec;
  if (!std::filesystem::is_directory(legacy, ec)) {
    return;
  }
  std::filesystem::create_directories(root, ec);
  size_t moved = 0;
  for (auto& entry : std::filesystem::directory_iterator(legacy, ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    auto dest = root / entry.path().filename();
    if (std::filesystem::exists(dest, ec)) {
      std::filesystem::remove(entry.path(), ec);
      continue;
    }
    std::filesystem::rename(entry.path(), dest, ec);
    if (ec) {  // cross-volume: copy + delete
      ec.clear();
      std::filesystem::copy_file(entry.path(), dest, ec);
      if (!ec) {
        std::filesystem::remove(entry.path(), ec);
      }
    }
    if (!ec) {
      ++moved;
    }
  }
  std::filesystem::remove(legacy, ec);  // only succeeds if now empty
  if (moved) {
    REXKRNL_INFO("[svc] migrated {} title-storage blob(s) from ./rexglue_server to {}", moved,
                 root.string());
  }
}

static bool StoreBlob(const std::string& path, const std::string& data) {
  MigrateLegacyBlobStore();
  std::error_code ec;
  std::filesystem::create_directories(BlobRoot(), ec);
  return !ec && LgWriteFileAtomic(BlobPath(path), data);
}

static bool LoadBlob(const std::string& path, std::string& out) {
  MigrateLegacyBlobStore();
  std::ifstream f(BlobPath(path), std::ios::binary);
  if (!f) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

// In-process service emulator. Decides the response for one (verb, path)
// request by path and logs every request under [svc].
static void ServeRequest(XHttpRequest& r) {
  const std::string verb = r.verb.empty() ? "GET" : r.verb;
  r.read_offset = 0;
  r.body.clear();
  if (r.path.find("/livegamer/") != std::string::npos) {
    std::lock_guard<std::mutex> lock(g_econ_mu);
    Economy();
    if (!g_econ_ready) { r.status = 503; return; }
  }
  if (r.path.find("cert/config.xml") != std::string::npos) {
    // LiveGamer bootstrap (XML despite the parser's "Json" name), parsed by
    // KERNELCOMMANDSXA__ApplyLgInfoJson; sets the lgServer/lgAuthEndpoint/
    // progressEndpoint globals that gate the store/unlock chain.
    // Also loads the economy so economy.xml exists from boot: the game-side
    // profile sync (src/sentient.cpp) only reads the file.
    {
      std::lock_guard<std::mutex> lock(g_econ_mu);
      Economy();
    }
    r.status = 200;
    r.body = BuildLgConfigXml();
  } else if (r.path.find("cert/additional_content.xml") != std::string::npos) {
    // Beenox additional-content manifest, fetched by AdditionalContentXA::Start
    // and parsed by sub_82B6BA28 (rapidxml). All attrs required unless noted;
    // root "version" must be fully numeric; "hash" must be exactly 64 hex
    // chars or the parser fails with "end badly":
    //   <additional_content version="N">
    //     <content name="..." contentId="N" description="...">
    //       <file filename="..." localname="..."(opt) parts="N"(opt) hash="64hex"/>
    //     </content>...
    //   </additional_content>
    // CONTENTMANAGERLZMA::UpdateCheckContent matches each <content name=> against
    // the `name` file of installed XContent packages (content root); installed
    // items are skipped (ERROR_ALREADY_EXISTS path). The manifest also drives
    // loading of installed content (AdditionalContentXA state 11->12): the
    // hash-verify worker (base 0x829C46B8, thread thunk 0x829C499C) opens
    // "<name>:\\<localname>" inside the mounted package and compares
    // XeCryptSha256(pak bytes || salt) against `hash`, where salt is the
    // 32-char string at CONTENTMANAGERLZMA+68 ("ab2nqijksuyeo3719306n024nahlit06"
    // for DCC2). localname/hash must match the real pak inside the installed
    // package: a bad localname strands the flow in state 12 (open fails, the
    // completion callback never fires); a bad hash fails verify (record
    // status=1 -> state 14 -> "end badly" retry loop that also blocks boot at
    // the splash). On success LoadAdditionalContentCallback rebuilds
    // LEVELCONFIGMANAGER, which is how the Hong Kong DLC levels reach the
    // native course-select.
    // Do not list patch_1/patch_2/dlc_tu1/dlc_tu2 (type-1 data paks): their
    // scripts add a TMS server-messages system that polls cert/messages.json
    // and gates the title-screen connect on parsing it ("an error occurred
    // while connecting" on A-press). Hong Kong alone is the supported set.
    // Only list content installed under the emulated content root: a
    // listed-but-missing entry enters the to-download vector and stalls in
    // the unemulated download flow. filename/localname = the pak's name inside
    // the STFS package (the avatar extractor installs it verbatim); hash =
    // SHA-256 of pak+salt (above).
    r.status = 200;
    r.body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<additional_content version=\"1\">"
        "<content name=\"dlc_hongkong\" contentId=\"1\" description=\"Hong Kong Fun Pack\">"
        "<file filename=\"dlc_hongkong.pak\" localname=\"dlc_hongkong.pak\" parts=\"1\" hash=\""
        "e91134f43f171cfaa0c46a4e4ae47e28005e7338393e23eea2a4122c262f9e9b\"/>"
        "</content>"
        "</additional_content>\r\n";
  } else if (r.path.find("cert/messages.json") != std::string::npos) {
    // TMS (title message system / MOTD) fetch, added by the patch_1/patch_2
    // data paks' scripts ("TMS::ParseMessages" is not in the base xex).
    // Polled every ~5s from the title screen; a 404/empty body fails the parse
    // ("Can't parse: Text only contains white space(s)") and leaves the title
    // "server unreachable": A then shows "an error occurred while connecting"
    // without issuing a request. Serve an empty but parseable message list.
    r.status = 200;
    r.body = "{\"messages\":[]}";
  } else if (r.path.find("store/items") != std::string::npos) {
    // Two consumers share this endpoint, distinguished by the query:
    //  - itemTypeNames=Vanity -> the shop Avatar Effects tab (items matched by
    //    id attribute against VANITYMANAGER ItemIds);
    //  - p.theme=<theme> -> the per-theme level catalog (items matched by the
    //    "id" property). Served as the full 28-level superset -- see kLgLevels.
    r.status = 200;
    r.body = r.path.find("itemTypeNames=Vanity") != std::string::npos
                 ? BuildLgVanityItemsXml()
                 : BuildLgStoreItemsXml();
  } else if (r.path.find("coin_offers.xml") != std::string::npos) {
    // Shop Coins category. Parser (sub_822C9D58) requires a <coinOffers> root;
    // without it ("Invalid CoinOffers.xml format, need coinOffers element") the
    // task returns 4 = FAILED and stalls Shop::Refresh's sequential task tree
    // before the Courses store/items fetches. An empty root parses (no
    // <coinOffer offerId= assetId= cost= amount= bonus=/> children needed; coin
    // purchases stay disabled per config.xml).
    r.status = 200;
    r.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<coinOffers/>\r\n";
  } else if (r.path.find("forexOffers") != std::string::npos) {
    // Shop consumable categories (rewind/powerup/jinx). Request carries
    // "properties=type%3D<type>" (one GET per category); parsers
    // (sub_822CF9A8 powerups / sub_822CFFC8 jinxes -- identical except the
    // manager-record name offset rec+116 vs rec+112) want:
    //   <forexOffers>
    //     <forexOffer id preferredFromAmount preferredToAmount>
    //       <fromCurrency name="CCSTARS"/><toCurrency name="CCP..."/>
    //     </forexOffer>...
    // Each offer's toCurrency@name must match a local manager record (the
    // modifier.wbr/jinx.wbr Currency Name values in kLgConsumables) or the
    // title logs "Forex offer with id N is not properly configured as an
    // item" and drops it. The LiveGamer API is XML throughout despite the
    // JSON_* names: JSON_Parse_Guarded 0x82244890 is a setjmp-guarded
    // rapidxml parse.
    std::string type;
    for (const char* key : {"type%3D", "type%3d", "type="}) {
      const size_t at = r.path.find(key);
      if (at != std::string::npos) {
        const size_t start = at + std::strlen(key);
        size_t end = start;
        while (end < r.path.size() && std::isalnum(static_cast<unsigned char>(r.path[end]))) {
          ++end;
        }
        type = r.path.substr(start, end - start);
        break;
      }
    }
    std::string offers;
    uint32_t count = 0;
    for (const auto& c : kLgConsumables) {
      if (type != c.type) {
        continue;
      }
      offers += "<forexOffer id=\"" + std::to_string(c.offer_id) + "\" preferredFromAmount=\"" +
                std::to_string(c.price_stars) + "\" preferredToAmount=\"" +
                std::to_string(c.qty) + "\"><fromCurrency name=\"CCSTARS\"/><toCurrency name=\"" +
                c.currency + "\"/></forexOffer>";
      ++count;
    }
    REXKRNL_DEBUG("[lgecon] forexOffers type='{}' -> {} offer(s)", type, count);
    r.status = 200;
    r.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<forexOffers>" + offers +
             "</forexOffers>\r\n";
  } else if (r.path.find("/purchaseOrder") != std::string::npos) {
    if (verb == "POST") {
      StoreBlob(r.path, r.upload_body);
      std::lock_guard<std::mutex> lock(g_econ_mu);
      auto& economy = Economy();
      auto next = economy;
      std::string reason;
      if (!LgPurchase(next, r.upload_body, LgUnlockPrice, reason)) {
        r.status = 400;
        REXKRNL_WARN("[lgecon] purchase declined: {}", reason);
      } else if (!next.Save(EconomyPath())) {
        r.status = 503;
        REXKRNL_ERROR("[lgecon] purchase not committed: economy save failed");
      } else {
        REXKRNL_INFO("[lgecon] purchase committed: stars {} -> {}, coins {} -> {}, rewinds {} -> {}",
                     economy.stars, next.stars, economy.coins, next.coins,
                     economy.rewinds, next.rewinds);
        economy = std::move(next);
        r.status = 200;
        r.body = "<purchaseOrder><orderState>CHARGED</orderState></purchaseOrder>";
      }
    } else {
      r.status = 200;
      r.body = "<purchaseOrders/>";
    }
  } else if (r.path.find("/itemInstances") != std::string::npos) {
    // Owned-unlocks query (builder sub_82B00DF8: GET %s/%s/itemInstances?
    // &appId=&itemTypeId=&ownerId=...). Parser 0x822F4AD8 unlocks each level
    // whose LgUnlockId/LgSplitPathId matches an itemInstance@itemId.
    std::lock_guard<std::mutex> lock(g_econ_mu);
    r.status = 200;
    r.body = BuildLgItemInstancesXml();
  } else if (r.path.find("/currencyTransfers") != std::string::npos && verb == "POST") {
    static std::atomic<uint32_t> transfer_seq{0};
    StoreBlob(r.path + "." + std::to_string(transfer_seq.fetch_add(1)), r.upload_body);
    std::lock_guard<std::mutex> lock(g_econ_mu);
    auto& economy = Economy();
    auto next = economy;
    std::string reason;
    if (!LgTransfers(next, r.upload_body, reason)) {
      r.status = 400;
      REXKRNL_WARN("[lgecon] transfer declined: {}", reason);
    } else if (!next.Save(EconomyPath())) {
      r.status = 503;
      REXKRNL_ERROR("[lgecon] transfer not committed: economy save failed");
    } else {
      REXKRNL_INFO("[lgecon] transfer committed: stars {} -> {}, total {} -> {}, coins {} -> {}, rewinds {} -> {}",
                   economy.stars, next.stars, economy.total_stars, next.total_stars,
                   economy.coins, next.coins, economy.rewinds, next.rewinds);
      economy = std::move(next);
      r.status = 200;
      r.body = r.upload_body;
    }
  } else if (r.path.find("/user/properties/") != std::string::npos) {
    // PUT user/properties/<userId>=coinPurchaseCounter etc. (builder
    // 0x82B00B90). Persist and acknowledge.
    if (verb == "PUT" || verb == "POST") {
      StoreBlob(r.path, r.upload_body);
    }
    r.status = 200;
    r.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<response/>\r\n";
  } else if (r.path.size() >= 5 &&
             (r.path.rfind("/user") == r.path.size() - 5 ||
              r.path.find("/user?") != std::string::npos)) {
    // LiveGamer account + balances. GET fetches; a 404 would make the title
    // POST an account-create to the same path (handler 0x822981A8), whose
    // response must also parse as a <user> document, so both verbs serve the
    // current economy state.
    std::lock_guard<std::mutex> lock(g_econ_mu);
    r.status = 200;
    r.body = BuildLgUserXml();
  } else if (r.path.find("additional_content.xml") != std::string::npos) {
    // Separate AdditionalContentXA download flow (not in the shop chain);
    // schema unknown. An empty valid document keeps it advancing.
    r.status = 200;
    r.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<response/>\r\n";
  } else if (verb == "GET" && r.path.find("/progress/users") != std::string::npos &&
             r.path.find("/achievements") != std::string::npos) {
    // sub_823039A8 parses this body as JSON (rapidjson) without checking the parse, then walks the
    // "achievements" array; a non-JSON body makes the walk read stack garbage and run across memory.
    r.status = 200;
    r.body = "{\"achievements\":[]}";
  } else if (verb == "GET" && (r.path.find("/livegamer") != std::string::npos ||
                               r.path.find("/progress") != std::string::npos ||
                               r.path.find("/lgauth") != std::string::npos)) {
    // Any other LiveGamer / progress-endpoint GET (per-level progress, ...):
    // answer 200 with an empty valid document instead of 404 so the chain keeps
    // advancing; the request still appears in the [svc] log. These prefixes
    // are the virtual hosts from BuildLgConfigXml (lgServer/lgAuthEndpoint/
    // progressEndpoint), so this cannot shadow a Title Storage blob path.
    r.status = 200;
    r.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<response/>\r\n";
  } else if (verb == "PUT" || verb == "POST") {
    // Uploads (profile saves, stats, gateway posts): accept + persist so a later
    // GET returns the same bytes -- a real Title Storage round-trip.
    r.status = StoreBlob(r.path, r.upload_body) ? 200 : 503;
  } else {
    // GET/HEAD/DELETE: serve a previously stored blob, else 404 (Title Storage
    // clients treat a missing blob as "use defaults" and proceed).
    r.status = LoadBlob(r.path, r.body) ? 200 : 404;
  }
  REXKRNL_DEBUG("[svc] {} {} -> {} ({} up / {} down)", verb, r.path, r.status, r.upload_body.size(),
               r.body.size());
}

// XHttpStartup(caller, reserved, reserved_ptr) -> BOOL.
u32 NetDll_XHttpStartup_entry(u32 caller, u32 reserved, u32 reserved_ptr) {
  return 1;
}

// XHttpShutdown(caller).
void NetDll_XHttpShutdown_entry(u32 caller) {}

// XHttpOpen(caller, agent, access_type, proxy, proxy_bypass, flags) -> hSession.
u32 NetDll_XHttpOpen_entry(u32 caller, mapped_string agent, u32 access_type,
                           mapped_string proxy, mapped_string proxy_bypass, u32 flags) {
  REXKRNL_DEBUG("XHttpOpen(agent='{}', access_type={}, flags={:#x})",
                agent ? static_cast<const char*>(agent) : "", access_type, flags);
  return alloc_xhttp_handle();
}

// XHttpConnect(caller, hSession, host, port, flags) -> hConnect.
u32 NetDll_XHttpConnect_entry(u32 caller, u32 session_handle, mapped_string host, u32 port,
                              u32 flags) {
  REXKRNL_DEBUG("XHttpConnect(session={:#x}, host='{}', port={})", session_handle,
                host ? static_cast<const char*>(host) : "", port);
  return alloc_xhttp_handle();
}

// XHttpOpenRequest(caller, hConnect, verb, path, version, referrer, accept_types, flags)
//   -> hRequest.
u32 NetDll_XHttpOpenRequest_entry(u32 caller, u32 connect_handle, mapped_string verb,
                                  mapped_string path, mapped_string version,
                                  mapped_string referrer, u32 accept_types, u32 flags) {
  const uint32_t handle = alloc_xhttp_handle();
  XHttpRequest req;
  req.verb = verb ? static_cast<const char*>(verb) : "GET";
  req.path = path ? static_cast<const char*>(path) : "";
  REXKRNL_DEBUG("XHttpOpenRequest(connect={:#x}) {} {} -> request={:#x}", connect_handle, req.verb,
                req.path, handle);
  {
    std::lock_guard<std::mutex> lock(g_xhttp_mu);
    g_requests[handle] = std::move(req);
  }
  return handle;
}

// XHttpSetStatusCallback(caller, handle, callback, flags, reserved) -> prev callback.
u32 NetDll_XHttpSetStatusCallback_entry(u32 caller, u32 handle, u32 callback_ptr, u32 flags,
                                        u32 reserved) {
  REXKRNL_DEBUG("XHttpSetStatusCallback(handle={:#x}, callback={:#x}, flags={:#x})", handle,
                callback_ptr, flags);
  std::lock_guard<std::mutex> lock(g_xhttp_mu);
  auto it = g_requests.find(handle);
  if (it != g_requests.end()) {
    it->second.callback = callback_ptr;
  }
  return 1;
}

// XHttpSendRequest(caller, hRequest, headers, header_len, optional, optional_len,
//                  total_len, context) -> BOOL. Produces the in-process response.
u32 NetDll_XHttpSendRequest_entry(u32 caller, u32 request_handle, mapped_string headers,
                                  u32 header_len, u32 optional_ptr, u32 optional_len,
                                  u32 total_len, u32 context) {
  REXKRNL_DEBUG("XHttpSendRequest(request={:#x}, header_len={}, context={:#x})", request_handle,
                header_len, context);
  std::lock_guard<std::mutex> lock(g_xhttp_mu);
  auto it = g_requests.find(request_handle);
  if (it == g_requests.end()) {
    return 0;
  }
  it->second.context = context;
  // Capture the request body (PUT/POST payload) from the optional buffer so the
  // emulated Title Storage can persist it.
  it->second.upload_body.clear();
  if (optional_ptr && optional_len) {
    const auto* p = REX_KERNEL_MEMORY()->TranslateVirtual<const char*>(optional_ptr);
    if (p) {
      it->second.upload_body.assign(p, optional_len);
    }
  }
  ServeRequest(it->second);
  // Async: the request is "in flight"; completion is age-gated to emulate one
  // network round-trip (see xhttp_response_delay_ms).
  const int delay = std::max(0, REXCVAR_GET(xhttp_response_delay_ms));
  g_notifications.push_back({request_handle, kCbSendRequestComplete, 0, 0, 0,
                             delay ? XHttpNowMs() + uint64_t(delay) : 0});
  return 1;
}

// XHttpReceiveResponse(caller, hRequest, reserved) -> BOOL.
// Async: queues HEADERS_AVAILABLE; the title's callback then issues its read
// (it does not poll QueryDataAvailable).
u32 NetDll_XHttpReceiveResponse_entry(u32 caller, u32 request_handle, u32 reserved) {
  REXKRNL_DEBUG("XHttpReceiveResponse(request={:#x})", request_handle);
  std::lock_guard<std::mutex> lock(g_xhttp_mu);
  auto it = g_requests.find(request_handle);
  if (it == g_requests.end()) {
    return 0;
  }
  // The title's XAUTHMANAGER callback is state-machine driven: HEADERS_AVAILABLE
  // sets the "headers ready" state and its loop then issues the read. It treats
  // DATA_AVAILABLE (0x40000) as an invalid state, so it is never sent.
  g_notifications.push_back({request_handle, kCbHeadersAvailable, 0, 0, 0});
  return 1;
}

// XHttpReadData(caller, hRequest, buffer, bytes_to_read, bytes_read_ptr) -> BOOL.
// Async: copies up to bytes_to_read into the guest buffer now and reports the
// count via a READ_COMPLETE notification on the next DoWork (lpvStatusInformation
// = buffer, dwStatusInformationLength = bytes read; 0 bytes => end of data).
u32 NetDll_XHttpReadData_entry(u32 caller, u32 request_handle, u32 buffer_guest,
                               u32 bytes_to_read, u32 bytes_read_ptr) {
  REXKRNL_DEBUG("XHttpReadData(request={:#x}, buffer={:#x}, to_read={})", request_handle,
                buffer_guest, bytes_to_read);
  std::lock_guard<std::mutex> lock(g_xhttp_mu);
  auto it = g_requests.find(request_handle);
  if (it == g_requests.end()) {
    return 0;
  }
  auto& r = it->second;
  uint32_t n = 0;
  if (buffer_guest) {
    const size_t remaining = r.body.size() - r.read_offset;
    n = static_cast<uint32_t>(std::min<size_t>(bytes_to_read, remaining));
    if (n) {
      std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(buffer_guest),
                  r.body.data() + r.read_offset, n);
      r.read_offset += n;
    }
  }
  if (bytes_read_ptr) {
    *REX_KERNEL_MEMORY()->TranslateVirtual<rex::be<uint32_t>*>(bytes_read_ptr) = n;
  }
  // Async: report the read result. A subsequent read returning 0 bytes signals
  // EOF (READ_COMPLETE with length 0). No DATA_AVAILABLE — see ReceiveResponse.
  g_notifications.push_back({request_handle, kCbReadComplete, buffer_guest, n, 0});
  return 1;
}

// XHttpQueryHeaders(caller, hRequest, info_level, name, buffer, buffer_len_ptr, index) -> BOOL.
u32 NetDll_XHttpQueryHeaders_entry(u32 caller, u32 request_handle, u32 info_level,
                                   mapped_string name, mapped_void buffer,
                                   mapped_u32 buffer_len_ptr, mapped_u32 index_ptr) {
  REXKRNL_DEBUG("XHttpQueryHeaders(request={:#x}, info_level={:#x})", request_handle, info_level);
  std::lock_guard<std::mutex> lock(g_xhttp_mu);
  auto it = g_requests.find(request_handle);
  if (it == g_requests.end()) {
    return 0;
  }
  const uint32_t level = info_level & kQueryLevelMask;
  const bool as_number = (info_level & kQueryFlagNumber) != 0;
  auto& req = it->second;

  // The title (ATGHTTP) queries the status code first (level 0xFFFE here, also
  // accept the desktop WinHttp value 19) then the content length (level 9).
  // Both are requested as numbers.
  const bool is_status = (level == 0xFFFE) || (level == kQueryStatusCode);

  if (as_number) {
    const uint32_t value =
        is_status ? static_cast<uint32_t>(req.status) : static_cast<uint32_t>(req.body.size());
    const uint32_t buf_len = buffer_len_ptr ? uint32_t(*buffer_len_ptr) : 0;
    const bool ok = buffer && buffer_len_ptr && buf_len >= sizeof(uint32_t);
    REXKRNL_DEBUG("  QueryHeaders num: level={:#x} is_status={} value={} buf_len={} -> {}", level,
                  is_status, value, buf_len, ok ? "OK" : "FAIL");
    if (ok) {
      *reinterpret_cast<rex::be<uint32_t>*>(static_cast<uint8_t*>(buffer)) = value;
      *buffer_len_ptr = sizeof(uint32_t);
      return 1;
    }
    return 0;
  }

  // String form (e.g. status text): return the numeric value as text.
  const std::string s =
      is_status ? std::to_string(req.status) : std::to_string(req.body.size());
  if (buffer && buffer_len_ptr && uint32_t(*buffer_len_ptr) >= s.size() + 1) {
    std::memcpy(static_cast<uint8_t*>(buffer), s.c_str(), s.size() + 1);
    *buffer_len_ptr = static_cast<uint32_t>(s.size());
    return 1;
  }
  return 0;
}

// XHttpDoWork(caller, handle, reserved) -> BOOL.
// Pumps the async state machine: delivers the oldest queued completion to its
// request's status callback. The callback re-enters XHttp (ReceiveResponse/
// ReadData) and enqueues the next completion, so the lock is released around
// the callback.
u32 NetDll_XHttpDoWork_entry(u32 caller, u32 handle, u32 reserved) {
  REXKRNL_DEBUG("XHttpDoWork(handle={:#x})", handle);
  uint32_t delivered = 0;
  // Deliver one completion per pump (real async semantics): the title defers
  // its header processing, so draining the whole queue at once would push
  // DATA_AVAILABLE before it is ready to read.
  for (uint32_t guard = 0; guard < 1; ++guard) {
    XHttpNotification n;
    uint32_t callback = 0;
    uint32_t context = 0;
    {
      std::lock_guard<std::mutex> lock(g_xhttp_mu);
      if (g_notifications.empty()) {
        break;
      }
      // FIFO age gate: the front is the oldest entry; if its delivery time
      // hasn't arrived, deliver nothing this pump (ordering preserved).
      if (g_notifications.front().ready_at_ms &&
          g_notifications.front().ready_at_ms > XHttpNowMs()) {
        break;
      }
      n = g_notifications.front();
      g_notifications.erase(g_notifications.begin());
      auto it = g_requests.find(n.request_handle);
      if (it != g_requests.end()) {
        callback = it->second.callback;
        context = it->second.context;
        // DATA_AVAILABLE delivers a pointer to a DWORD byte-count. Stage it in
        // the request's guest scratch DWORD.
        if (n.status == kCbDataAvailable) {
          if (!it->second.scratch) {
            it->second.scratch = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(uint32_t));
          }
          *REX_KERNEL_MEMORY()->TranslateVirtual<rex::be<uint32_t>*>(it->second.scratch) =
              n.info_value;
          n.info_ptr = it->second.scratch;
          n.info_len = sizeof(uint32_t);
        }
      }
    }
    if (!callback) {
      continue;
    }
    PPCFunc* fn = rex::runtime::ResolveIndirectFunction(callback);
    if (!fn) {
      REXKRNL_WARN("XHttpDoWork: unresolved status callback {:#x}", callback);
      continue;
    }
    REXKRNL_DEBUG("XHttp callback(request={:#x}, status={:#x}, info={:#x}, len={})",
                  n.request_handle, n.status, n.info_ptr, n.info_len);
    // WINHTTP_STATUS_CALLBACK(hInternet, dwContext, dwInternetStatus,
    //                         lpvStatusInformation, dwStatusInformationLength)
    rex::ppc::GuestToHostFunction<void>(fn, n.request_handle, context, n.status, n.info_ptr,
                                        n.info_len);
    ++delivered;
  }
  // BOOL success, not a delivered count. The XAUTHMANAGER request driver
  // (sub_82B30990) calls DoWork after every state step and treats 0 as a hard
  // error (state 11, request silently dropped). Its post-header step (state
  // 5 -> 7) runs with an empty notification queue, so returning `delivered`
  // would kill every request before its body read. Idle pumps are normal.
  (void)delivered;
  return 1;
}

// XHttpCloseHandle(caller, handle) -> BOOL.
u32 NetDll_XHttpCloseHandle_entry(u32 caller, u32 handle) {
  std::lock_guard<std::mutex> lock(g_xhttp_mu);
  g_requests.erase(handle);
  return 1;
}

// XHttpQueryOption(caller, handle, option, buffer, buffer_len_ptr) -> BOOL.
u32 NetDll_XHttpQueryOption_entry(u32 caller, u32 handle, u32 option, mapped_void buffer,
                                  mapped_u32 buffer_len_ptr) {
  REXKRNL_DEBUG("XHttpQueryOption(handle={:#x}, option={})", handle, option);
  return 0;
}

// XHttpSetOption(caller, handle, option, buffer, buffer_len) -> BOOL.
u32 NetDll_XHttpSetOption_entry(u32 caller, u32 handle, u32 option, mapped_void buffer,
                                u32 buffer_len) {
  REXKRNL_DEBUG("XHttpSetOption(handle={:#x}, option={})", handle, option);
  return 1;
}

//=============================================================================
// XampXAuth* — Xbox authentication, started as part of the title's online
// login. Reports success so it proceeds (as xenia-canary does with
// network_mode=XBOXLIVE); unimplemented, the title enters a startup-retry loop
// that exhausts the guest heap.
//=============================================================================

// XampXAuthStartup(settings) -> X_ERROR_SUCCESS. Single arg = XAUTH_SETTINGS*.
u32 XampXAuthStartup_entry(u32 settings_ptr) {
  return 0;  // X_ERROR_SUCCESS
}

// XampXAuthShutdown(unkn) -> void. Writes 1 to *unkn (0 would request a
// follow-up XampXAuthGetTitleBuffer call).
void XampXAuthShutdown_entry(mapped_u32 unkn_ptr) {
  if (unkn_ptr) {
    *unkn_ptr = 1;
  }
}

// XampXAuthGetTitleBuffer() -> 0 (a non-zero pointer here would crash the title).
u32 XampXAuthGetTitleBuffer_entry() {
  return 0;
}

// XampXAuthIsLocalSocketAllowed() -> TRUE (permit SO_GRANTINSECURE sockets).
u32 XampXAuthIsLocalSocketAllowed_entry() {
  return 1;
}

//=============================================================================
// XamGetServiceEndpoint(service_name, out_endpoint, out_len, overlapped)
//   Resolves a named LIVE service to a URL. On hardware the Xbox LIVE service
//   directory returns the publisher's server; here it returns kServiceHost +
//   service_name so requests reach the in-process service emulator.
//=============================================================================

// Host the title is pointed at. Requests are intercepted in the XHttp layer, so
// this never has to be reachable on the network; the path carries the service.
static constexpr const char* kServiceHost = "http://dcc2.service.rexglue/";

u32 XamGetServiceEndpoint_entry(mapped_string service_name, mapped_string out_endpoint,
                                u32 out_endpoint_len, u32 overlapped_ptr) {
  if (!service_name || !out_endpoint || !out_endpoint_len) {
    return X_ERROR_INVALID_PARAMETER;
  }

  std::string name(static_cast<const char*>(service_name));
  std::string endpoint = std::string(kServiceHost) + name;
  REXKRNL_DEBUG("[svc] XamGetServiceEndpoint('{}') -> '{}'", name, endpoint);

  X_RESULT result;
  std::memset(static_cast<char*>(out_endpoint), 0, out_endpoint_len);
  if (out_endpoint_len < endpoint.size() + 1) {
    result = X_ERROR_INSUFFICIENT_BUFFER;
  } else {
    std::memcpy(static_cast<char*>(out_endpoint), endpoint.c_str(), endpoint.size() + 1);
    result = X_ERROR_SUCCESS;
  }

  if (overlapped_ptr) {
    rex::system::kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  }
  return result;
}

// XamGetToken(user_index, url, url_size, token_out, overlapped) -> result.
// Returns a mock relying-party (XSTS) token; the title attaches it to the
// Authorization header of the title-storage request. Nothing validates it.
// Mirrors xenia-canary's mock token.
u32 XamGetToken_entry(u32 user_index, mapped_string url, u32 url_size, mapped_u32 token_out,
                      u32 overlapped_ptr) {
  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* mem = REX_KERNEL_MEMORY();

  std::string url_str;
  if (url) {
    const char* u = static_cast<const char*>(url);
    url_str.assign(u, url_size ? url_size : std::strlen(u));
  }
  REXKRNL_DEBUG("[svc] XamGetToken(user={}, url='{}')", user_index, url_str);

  static const char kMockToken[] = "MOCK_XBOX_STS_TOKEN";
  const uint32_t token_len = static_cast<uint32_t>(sizeof(kMockToken) - 1);

  const uint32_t data_addr = mem->SystemHeapAlloc(token_len);
  std::memcpy(mem->TranslateVirtual<uint8_t*>(data_addr), kMockToken, token_len);

  const uint32_t token_addr = mem->SystemHeapAlloc(sizeof(cc2::kernel::XAM_RELYING_PARTY_TOKEN));
  auto* token = mem->TranslateVirtual<cc2::kernel::XAM_RELYING_PARTY_TOKEN*>(token_addr);
  token->reserved = 0;
  token->length = token_len;
  token->token_data_ptr = data_addr;

  if (token_out) {
    *token_out = token_addr;
  }

  if (overlapped_ptr) {
    rex::system::kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

// XHttpCrackUrl(caller, url, url_length, flags, components) -> BOOL.
// Parses scheme://host[:port][/path][?query] into the guest XHTTP_URL_COMPONENTS.
// Supports both modes the title may use per component: a caller-provided buffer
// (ptr set -> copy + NUL) or pointer-into-URL (ptr 0, length nonzero -> set ptr
// to point into the original URL). Sufficient for the endpoints
// XamGetServiceEndpoint hands out.
u32 NetDll_XHttpCrackUrl_entry(u32 caller, u32 url_guest, u32 url_length, u32 flags,
                               u32 components_guest) {
  auto* mem = REX_KERNEL_MEMORY();
  if (!url_guest || !components_guest) {
    return 0;
  }
  auto* comp = mem->TranslateVirtual<cc2::kernel::XHTTP_URL_COMPONENTS*>(components_guest);
  if (comp->struct_size != sizeof(cc2::kernel::XHTTP_URL_COMPONENTS)) {
    return 0;
  }

  const char* url_host = mem->TranslateVirtual<const char*>(url_guest);
  std::string url = url_length ? std::string(url_host, url_length) : std::string(url_host);
  REXKRNL_DEBUG("XHttpCrackUrl('{}')", url);

  size_t scheme_off = 0, scheme_len = 0, host_off = 0, host_len = 0;
  size_t path_off = 0, path_len = 0, query_off = 0, query_len = 0;
  uint16_t port = 0;
  uint32_t scheme_enum = 0;

  size_t cursor = 0;
  const size_t scheme_sep = url.find("://");
  if (scheme_sep != std::string::npos) {
    scheme_off = 0;
    scheme_len = scheme_sep;
    cursor = scheme_sep + 3;
    const std::string scheme = url.substr(0, scheme_len);
    if (scheme == "https") {
      scheme_enum = static_cast<uint32_t>(cc2::kernel::X_INTERNET_SCHEME::HTTPS);
      port = 443;
    } else {
      scheme_enum = static_cast<uint32_t>(cc2::kernel::X_INTERNET_SCHEME::HTTP);
      port = 80;
    }
  }

  const size_t host_start = cursor;
  const size_t path_start = url.find('/', cursor);
  const size_t hostport_end = (path_start == std::string::npos) ? url.size() : path_start;
  const std::string hostport = url.substr(host_start, hostport_end - host_start);
  const size_t colon = hostport.find(':');
  if (colon != std::string::npos) {
    host_off = host_start;
    host_len = colon;
    port = static_cast<uint16_t>(std::strtoul(hostport.c_str() + colon + 1, nullptr, 10));
  } else {
    host_off = host_start;
    host_len = hostport.size();
  }

  if (path_start != std::string::npos) {
    const size_t q = url.find('?', path_start);
    if (q != std::string::npos) {
      path_off = path_start;
      path_len = q - path_start;
      query_off = q;
      query_len = url.size() - q;
    } else {
      path_off = path_start;
      path_len = url.size() - path_start;
    }
  }

  comp->scheme = scheme_enum;
  comp->port = port;

  auto emit = [&](rex::be<uint32_t>& ptr_field, rex::be<uint32_t>& len_field, size_t off,
                  size_t len) -> bool {
    const uint32_t cur_ptr = ptr_field;
    const uint32_t cur_len = len_field;
    if (cur_ptr) {
      if (cur_len < len + 1) {
        len_field = static_cast<uint32_t>(len + 1);
        return false;
      }
      char* dst = mem->TranslateVirtual<char*>(cur_ptr);
      std::memcpy(dst, url.data() + off, len);
      dst[len] = '\0';
      len_field = static_cast<uint32_t>(len);
    } else if (cur_len) {
      ptr_field = url_guest + static_cast<uint32_t>(off);
      len_field = static_cast<uint32_t>(len);
    }
    return true;
  };

  bool ok = true;
  if (scheme_len) ok &= emit(comp->scheme_ptr, comp->scheme_length, scheme_off, scheme_len);
  if (host_len) ok &= emit(comp->host_name_ptr, comp->host_name_length, host_off, host_len);
  if (path_len) ok &= emit(comp->url_path_ptr, comp->url_path_length, path_off, path_len);
  if (query_len) ok &= emit(comp->extra_info_ptr, comp->extra_info_length, query_off, query_len);

  return ok ? 1 : 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__NetDll_XHttpStartup, rex::kernel::xam::NetDll_XHttpStartup_entry)
REX_EXPORT(__imp__NetDll_XHttpShutdown, rex::kernel::xam::NetDll_XHttpShutdown_entry)
REX_EXPORT(__imp__NetDll_XHttpOpen, rex::kernel::xam::NetDll_XHttpOpen_entry)
REX_EXPORT(__imp__NetDll_XHttpConnect, rex::kernel::xam::NetDll_XHttpConnect_entry)
REX_EXPORT(__imp__NetDll_XHttpOpenRequest, rex::kernel::xam::NetDll_XHttpOpenRequest_entry)
REX_EXPORT(__imp__NetDll_XHttpSetStatusCallback,
           rex::kernel::xam::NetDll_XHttpSetStatusCallback_entry)
REX_EXPORT(__imp__NetDll_XHttpSendRequest, rex::kernel::xam::NetDll_XHttpSendRequest_entry)
REX_EXPORT(__imp__NetDll_XHttpReceiveResponse,
           rex::kernel::xam::NetDll_XHttpReceiveResponse_entry)
REX_EXPORT(__imp__NetDll_XHttpReadData, rex::kernel::xam::NetDll_XHttpReadData_entry)
REX_EXPORT(__imp__NetDll_XHttpQueryHeaders, rex::kernel::xam::NetDll_XHttpQueryHeaders_entry)
REX_EXPORT(__imp__NetDll_XHttpDoWork, rex::kernel::xam::NetDll_XHttpDoWork_entry)
REX_EXPORT(__imp__NetDll_XHttpCloseHandle, rex::kernel::xam::NetDll_XHttpCloseHandle_entry)
REX_EXPORT(__imp__NetDll_XHttpQueryOption, rex::kernel::xam::NetDll_XHttpQueryOption_entry)
REX_EXPORT(__imp__NetDll_XHttpSetOption, rex::kernel::xam::NetDll_XHttpSetOption_entry)

REX_EXPORT(__imp__XampXAuthStartup, rex::kernel::xam::XampXAuthStartup_entry)
REX_EXPORT(__imp__XampXAuthShutdown, rex::kernel::xam::XampXAuthShutdown_entry)
REX_EXPORT(__imp__XampXAuthGetTitleBuffer, rex::kernel::xam::XampXAuthGetTitleBuffer_entry)
REX_EXPORT(__imp__XampXAuthIsLocalSocketAllowed,
           rex::kernel::xam::XampXAuthIsLocalSocketAllowed_entry)

REX_EXPORT(__imp__XamGetServiceEndpoint, rex::kernel::xam::XamGetServiceEndpoint_entry)
REX_EXPORT(__imp__XamGetToken, rex::kernel::xam::XamGetToken_entry)
REX_EXPORT(__imp__NetDll_XHttpCrackUrl, rex::kernel::xam::NetDll_XHttpCrackUrl_entry)



namespace cc2::kernel::xam {
bool LgEconomyGrantItem(uint32_t item_id) {
  using namespace rex::kernel::xam;
  std::lock_guard<std::mutex> lock(g_econ_mu);
  auto& economy = Economy();
  if (!g_econ_ready) return false;
  if (economy.owned.count(item_id)) return true;
  auto next = economy;
  next.owned.insert(item_id);
  if (!next.Save(EconomyPath())) {
    REXKRNL_ERROR("[lgecon] native reward item {} could not be saved", item_id);
    return false;
  }
  economy = std::move(next);
  REXKRNL_INFO("[lgecon] native reward item {} committed", item_id);
  return true;
}
}  // namespace cc2::kernel::xam
