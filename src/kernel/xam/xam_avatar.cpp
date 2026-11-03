/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Ported xenia-canary xam_avatar into ReXGlue.
 *
 * Port of xenia-canary's xam_avatar.cc. The avatar APIs complete their
 * overlapped requests (success, or clean failure for asset/manifest queries)
 * so titles that show the player's avatar (e.g. DCC2's menu "gui/gamepad0"
 * XAVT resource) can initialise the avatar pipeline offline instead of
 * crashing on a half-initialised state. The local manifest is persisted to a
 * host file and also backs XPROFILE_GAMERCARD_AVATAR_INFO_1.
 */

#include <algorithm>
#include <functional>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include "cc2/kernel/title_id_utils.h"
#include <rex/kernel/xam/private.h>
#include "cc2/kernel/xam/local_players.h"
#include <rex/filesystem.h>
#include "cc2/kernel/xam/xam_avatar.h"
#include "cc2/lobby.h"
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "avatars/asset_pack.h"
#include "avatars/closet.h"
#include "avatars/extra_component.h"
#include "avatars/guest_animation.h"
#include "avatars/guest_asset.h"
#include "avatars/guest_load_animation.h"
#include "avatars/guest_load_asset.h"
#include "avatars/memory_block.h"

// game_data_root is defined in the runtime (runtime.cpp, global namespace);
// read here to locate the DCC2 AI avatar looks under data/art/_avatar/.
REXCVAR_DECLARE(std::string, game_data_root);

// Titles that need a full avatar implementation may crash with this on; it
// defaults on so avatar-using titles boot.
REXCVAR_DEFINE_BOOL(allow_avatar_initialization, true, "Kernel",
                    "Enable Avatar Initialization (some games need a full avatar impl)");

// Host directory containing the extracted avatar asset-pack TOCs
// (AvatarAssetPack.toc + AvatarAssetPackLegacyV1.toc), extracted from the
// FFFE07DF00000002 PIRS package. Empty => <user_data_root>\avatarpack.
REXCVAR_DEFINE_STRING(avatar_asset_pack_dir, "", "Kernel",
                      "Directory with extracted AvatarAssetPack*.toc files. Empty = "
                      "<user_data_root>\\avatarpack (the shared "
                      "Documents\\JMstudios\\userdata\\avatarpack by default).");

// The closet (imported marketplace/award avatar items) can live apart from
// the pack so it ships with a title's assets folder: saved outfits reference
// closet items by GUID, so a machine without the same closet hides those
// outfits from the editor's grids. A relative path resolves against
// game_data_root (same convention as content_root).
REXCVAR_DEFINE_STRING(avatar_closet_dir, "", "Kernel",
                      "Directory with imported closet items (<guid>.bin + closet_index.tsv, "
                      "built by avatarextract --closet-import). Empty = "
                      "<avatar_asset_pack_dir>\\closet. Relative paths resolve against "
                      "game_data_root.");

namespace rex {
namespace kernel {
namespace xam {

using cc2::kernel::kAvatarEditorID;
using cc2::kernel::xam::X_ASSET_ID;
using cc2::kernel::xam::X_AVATAR_BODY_TYPE;
using cc2::kernel::xam::X_AVATAR_METADATA;

// --- Avatar asset pipeline state --------------------------------------------
static avatars::AssetPack g_avatar_asset_pack;
static avatars::AssetPack g_legacy_avatar_asset_pack;
static uint32_t g_title_version = 0;
static uint32_t g_coordinate_system = 0;

// Local player's manifest (big-endian guest bytes), from the persisted file or
// the player's GetAssets build. GetManifestLocalUser replays it so the menu
// portrait gets a real manifest instead of "no avatar".
static uint8_t g_captured_metadata[1000];
static bool g_have_metadata = false;
// Guest address of the buffer the game last asked GetManifestLocalUser to fill
// (DCC2's 1000-byte metadata buffer). GetAssets copies the manifest into it so
// AVATARMODELMASTER's "metadata not set" retry succeeds when the manifest read
// preceded GetAssets.
static uint32_t g_last_manifest_dest = 0;

// Per-CWD manifest cache, read-only fallback when the shared manifest is absent.
static const char* kLegacyMetaCachePath = "dcc2_avatar_meta.bin";

REXCVAR_DEFINE_STRING(avatar_data_dir, "", "Kernel",
                      "Shared cross-title folder for persisted avatar data: the manifest the "
                      "Avatar Editor saves and every game then loads as the local user's "
                      "avatar. Empty = <user_data_root>\\avatars (the shared "
                      "Documents\\JMstudios\\userdata\\avatars by default).");

static std::string AvatarManifestPath() {
  std::string dir = REXCVAR_GET(avatar_data_dir);
  if (dir.empty()) {
    // Live inside the shared user-data tree so saves + avatar travel as one
    // folder (and any user_data_root override carries the avatar with it).
    const auto& root = REX_KERNEL_STATE()->emulator()->user_data_root();
    dir = root.empty() ? std::string("avatars") : (root / "avatars").string();
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir + "\\avatar_manifest.bin";
}

static void WriteHostFile(const std::string& path, const uint8_t* data, size_t size) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    return;
  }
  std::fwrite(data, 1, size, f);
  std::fclose(f);
}

static std::vector<uint8_t> ReadHostFile(const std::string& path) {
  std::vector<uint8_t> data;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return data;
  }
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    data.resize(static_cast<size_t>(size));
    size_t read = std::fread(data.data(), 1, data.size(), f);
    data.resize(read);
  }
  std::fclose(f);
  return data;
}

// Read the persisted manifest: shared cross-title location first, then the
// legacy per-CWD cache file.
static std::vector<uint8_t> ReadPersistedManifest(std::string* out_path) {
  std::string path = AvatarManifestPath();
  auto data = ReadHostFile(path);
  if (data.empty()) {
    path = kLegacyMetaCachePath;
    data = ReadHostFile(path);
  }
  if (out_path) {
    *out_path = path;
  }
  return data;
}

// Titles like Crash Course 2 read the avatar manifest from the profile
// (XamUserReadProfileSettings, XPROFILE_GAMERCARD_AVATAR_INFO_1 = 0x63E80044)
// rather than through XamAvatarGetManifestLocalUser. Back that setting with the
// persisted manifest, re-read from disk so a mid-session save is picked up.
static bool LoadAvatarAssetPack();
static void BuildRandomAvatarMetadata(avatars::X_AVATAR_METADATA* out, uint32_t body_mask,
                                      uint32_t salt);

static void EnsureAvatarProfileSettingImpl(system::xam::UserProfile* profile) {
  if (!profile) {
    return;
  }
  constexpr uint32_t kAvatarInfo1SettingId = 0x63E80044u;
  auto manifest = ReadPersistedManifest(nullptr);
  if (manifest.size() != sizeof(avatars::X_AVATAR_METADATA) && LoadAvatarAssetPack()) {
    // No persisted avatar. Consoles give new profiles a random avatar, so
    // synthesize one (title preset manifests preferred) and persist it; the
    // Avatar Editor can customize it later.
    avatars::X_AVATAR_METADATA random_meta;
    BuildRandomAvatarMetadata(&random_meta, 3, 1);
    random_meta.owner_xuid = profile->xuid();  // player-owned, not an NPC build
    const auto* bytes = reinterpret_cast<const uint8_t*>(&random_meta);
    manifest.assign(bytes, bytes + sizeof(random_meta));
    WriteHostFile(AvatarManifestPath(), manifest.data(), manifest.size());
    REXKRNL_INFO("[avatar] no persisted avatar; generated a default and saved it to {}",
                 AvatarManifestPath());
  }
  if (manifest.size() != sizeof(avatars::X_AVATAR_METADATA)) {
    return;
  }
  auto* setting = profile->GetSetting(kAvatarInfo1SettingId);
  if (setting) {
    setting->Deserialize(std::move(manifest));
  } else {
    profile->AddSetting(std::make_unique<system::xam::UserProfile::BinarySetting>(
        kAvatarInfo1SettingId, manifest));
    REXKRNL_DEBUG("[avatar] profile setting {:08X} backed by persisted avatar manifest",
                 kAvatarInfo1SettingId);
  }
}

static bool LoadAvatarAssetPack() {
  if (g_avatar_asset_pack.is_loaded()) {
    return true;
  }
  std::string dir = REXCVAR_GET(avatar_asset_pack_dir);
  if (dir.empty()) {
    // Portable default: the pack lives inside the shared user-data tree; the
    // cvar (settable from the title's toml) overrides for custom locations.
    const auto& root = REX_KERNEL_STATE()->emulator()->user_data_root();
    if (!root.empty()) {
      dir = (root / "avatarpack").string();
    }
  }
  if (dir.empty()) {
    REXKRNL_WARN("[avatar] avatar_asset_pack_dir not set; avatar assets unavailable");
    return false;
  }
  auto toc = ReadHostFile(dir + "/AvatarAssetPack.toc");
  const size_t toc_size = toc.size();  // Load() swaps the buffer away.
  if (!toc.empty() && g_avatar_asset_pack.Load(toc)) {
    REXKRNL_INFO("[avatar] loaded AvatarAssetPack.toc ({} bytes) from {}", toc_size, dir);
  } else {
    REXKRNL_WARN("[avatar] failed to load AvatarAssetPack.toc from {}", dir);
  }
  auto legacy = ReadHostFile(dir + "/AvatarAssetPackLegacyV1.toc");
  if (!legacy.empty()) {
    g_legacy_avatar_asset_pack.Load(legacy);
    // The legacy pack doubles as the old->new body correspondence source for
    // the old-body item rescue (guest_load_asset.cpp).
    avatars::SetLegacyAssetPack(
        g_legacy_avatar_asset_pack.is_loaded() ? &g_legacy_avatar_asset_pack : nullptr);
  }
  // Imported marketplace/award items (avatarextract --closet-import): by
  // default a closet/ subdirectory next to the pack; avatar_closet_dir
  // relocates it (e.g. into the Avatar Editor's shipped assets folder).
  std::filesystem::path closet_dir = std::filesystem::path(dir) / "closet";
  const std::string closet_override = REXCVAR_GET(avatar_closet_dir);
  if (!closet_override.empty()) {
    closet_dir = closet_override;
    if (closet_dir.is_relative()) {
      const std::string game_root = REXCVAR_GET(game_data_root);
      if (!game_root.empty()) {
        closet_dir = std::filesystem::path(game_root) / closet_dir;
      }
    }
  }
  avatars::GetCloset().Load(closet_dir);
  return g_avatar_asset_pack.is_loaded();
}

// Start/End ------------------------------------------------------------------
// userdata/Avatar/Player<N>: a manifest .bin put there is local player N's avatar in place of the dev avatars.
static std::filesystem::path LocalPlayerAvatarDir(uint32_t user_index) {
  return rex::filesystem::GetUserFolder() / "JMstudios" / "userdata" / "Avatar" /
         ("Player" + std::to_string(user_index + 1));
}

// The newest 1000-byte .bin in a local player's folder, rescanned every two seconds; false when there is none.
static bool LocalPlayerManifest(uint32_t user_index, avatars::X_AVATAR_METADATA* out) {
  struct Cached {
    std::chrono::steady_clock::time_point checked{};
    bool found = false;
    avatars::X_AVATAR_METADATA manifest{};
  };
  static std::mutex mutex;
  static Cached cached[4];
  if (user_index == 0 || user_index >= 4) return false;
  std::lock_guard lock(mutex);
  Cached& entry = cached[user_index];
  const auto now = std::chrono::steady_clock::now();
  if (entry.checked == std::chrono::steady_clock::time_point{} || now - entry.checked >= std::chrono::seconds(2)) {
    entry.checked = now;
    const bool had = entry.found;
    entry.found = false;
    std::error_code ec;
    std::filesystem::path newest;
    std::filesystem::file_time_type newest_time{};
    for (auto it = std::filesystem::directory_iterator(LocalPlayerAvatarDir(user_index), ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
      std::string ext = it->path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      std::error_code entry_ec;
      if (ext != ".bin" || !it->is_regular_file(entry_ec) || it->file_size(entry_ec) != sizeof(entry.manifest)) {
        continue;
      }
      const auto time = it->last_write_time(entry_ec);
      if (newest.empty() || time > newest_time) newest = it->path(), newest_time = time;
    }
    if (!newest.empty()) {
      const auto data = ReadHostFile(newest.string());
      if (data.size() == sizeof(entry.manifest)) {
        std::memcpy(&entry.manifest, data.data(), sizeof(entry.manifest));
        entry.found = true;
      }
    }
    if (entry.found != had) {
      REXKRNL_INFO("[avatar] player {} avatar: {}", user_index + 1,
                   entry.found ? newest.string() : std::string("dev avatars"));
    }
  }
  if (entry.found && out) *out = entry.manifest;
  return entry.found;
}

u32 XamAvatarInitialize_entry(u32 version,            // 1, 3, 4, etc
                              u32 coordinate_system,  // 0 or 1
                              u32 processor_number,    // for thread creation?
                              mapped_u32 function_ptrs,  // 20b, 5 pointers
                              mapped_void unk5,          // data segment ptr
                              u32 unk6                   // flags - 0x00300000, 0x30
) {
  REXKRNL_DEBUG("[avatar] XamAvatarInitialize(version={}, cs={}, flags={:#x})", version,
                coordinate_system, unk6);
  {
    std::error_code ec;
    std::filesystem::create_directories(LocalPlayerAvatarDir(1), ec);
  }
  if (REX_KERNEL_STATE()->title_id() == kAvatarEditorID) {
    // The Avatar Editor skips the game-oriented init (function-pointer
    // capture) but still needs the version and asset pack, or every GetAssets
    // fails and the editor null-derefs. The persisted manifest makes it open
    // on the avatar last saved with "Save and Exit".
    g_title_version = version;
    g_coordinate_system = coordinate_system;
    LoadAvatarAssetPack();
    if (!g_have_metadata) {
      std::string manifest_path;
      auto cached = ReadPersistedManifest(&manifest_path);
      if (cached.size() == sizeof(g_captured_metadata)) {
        std::memcpy(g_captured_metadata, cached.data(), sizeof(g_captured_metadata));
        g_have_metadata = true;
        REXKRNL_INFO("[avatar] loaded persisted avatar manifest from {}", manifest_path);
      }
    }
    // The editor's save flow pumps an async validate-manifest + write-gamerpic
    // op whose pointer lives at app+0x224A1B0 (guest 0x94530040). Its creator
    // never runs here, so the "saving" screen would poll a null pointer and
    // crash. A zeroed placeholder suffices: every field the pump touches
    // (status @+12, XOVERLAPPED @+36, flags @+64/+72, buffer slots @dwords
    // 21..33) is plain data, and every terminal path on a zeroed op returns to
    // the menu.
    {
      auto* mem = REX_KERNEL_MEMORY();
      auto* op_field = mem->TranslateVirtual<rex::be<uint32_t>*>(0x94530040u);
      if (!*op_field) {
        const uint32_t op = mem->SystemHeapAlloc(160);
        if (op) {
          std::memset(mem->TranslateVirtual<uint8_t*>(op), 0, 160);
          *op_field = op;
          REXKRNL_INFO("[avatar] installed placeholder save-op object at {:#x}", op);
        }
      }
    }
    return X_STATUS_SUCCESS;
  }
  if (!REXCVAR_GET(allow_avatar_initialization)) {
    return ~0u;  // game calls XamAvatarShutdown and runs avatar-less.
  }
  g_title_version = version;
  g_coordinate_system = coordinate_system;
  LoadAvatarAssetPack();  // best-effort; GetAssets fails gracefully if absent.
  // Load the persisted manifest so valid metadata exists before the master
  // builds the avatar; without it the avatar builds hollow (invisible).
  if (!g_have_metadata) {
    std::string manifest_path;
    auto cached = ReadPersistedManifest(&manifest_path);
    if (cached.size() == sizeof(g_captured_metadata)) {
      std::memcpy(g_captured_metadata, cached.data(), sizeof(g_captured_metadata));
      g_have_metadata = true;
      REXKRNL_INFO("[avatar] loaded cached avatar metadata from {}", manifest_path);
    }
  }
  return X_STATUS_SUCCESS;
}

void XamAvatarShutdown_entry() {
  // No-op. (Real: XMsgStartIORequestEx(0xf3,0x600002,...) / XamUnloadSysApp.)
}

// Get & Set ------------------------------------------------------------------
u32 XamAvatarGetManifestLocalUser_entry(u32 user_index,
                                        ppc_ptr_t<X_AVATAR_METADATA> avatar_metadata_ptr,
                                        mapped_void overlapped_ptr) {
  REXKRNL_DEBUG("[avatar] XamAvatarGetManifestLocalUser(user={})", user_index);
  // Remember where the game wants player 1's manifest written so GetAssets can fill it
  // once it has the real metadata (the read usually happens before GetAssets).
  if (avatar_metadata_ptr && user_index == 0) {
    g_last_manifest_dest = avatar_metadata_ptr.guest_address();
    // With metadata already loaded, fill it synchronously so the master reads
    // a real manifest, not a hollow one.
    if (g_have_metadata) {
      std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(g_last_manifest_dest),
                  g_captured_metadata, sizeof(g_captured_metadata));
    }
  }
  auto run = [=](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    extended_error = X_ERROR_SUCCESS;
    length = 0;

    if (user_index >= 4) {
      extended_error = X_E_INVALIDARG;
      return X_ERROR_INVALID_PARAMETER;
    }
    if (!avatar_metadata_ptr) {
      extended_error = X_E_INVALIDARG;
      return X_ERROR_INVALID_PARAMETER;
    }
    // Players 2-4 with a manifest in their avatars folder get it under their own XUID.
    if (user_index) {
      avatars::X_AVATAR_METADATA manifest;
      if (LocalPlayerManifest(user_index, &manifest)) {
        manifest.owner_xuid = cc2::kernel::xam::LocalPlayerXuid(user_index);
        std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(avatar_metadata_ptr.guest_address()), &manifest,
                    sizeof(manifest));
        return X_ERROR_SUCCESS;
      }
    }
    // Replay the local manifest so the menu portrait builds a real avatar
    // instead of falling back to "no avatar".
    if (g_have_metadata && avatar_metadata_ptr) {
      auto* out = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(
          avatar_metadata_ptr.guest_address());
      std::memcpy(out, g_captured_metadata, sizeof(g_captured_metadata));
      // Players 2-4 get it under their own XUID, so GetAssets deals them a dev avatar as it does the AI.
      if (user_index) {
        reinterpret_cast<avatars::X_AVATAR_METADATA*>(out)->owner_xuid =
            cc2::kernel::xam::LocalPlayerXuid(user_index);
      }
      return X_ERROR_SUCCESS;
    }
    extended_error = X_E_FAIL;
    return X_ERROR_FUNCTION_FAILED;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error = 0, length = 0;
    X_RESULT result = run(extended_error, length);
    return result == X_ERROR_SUCCESS ? result : extended_error;
  }

  REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped_ptr.guest_address());
  return X_ERROR_IO_PENDING;
}

u32 XamAvatarGetManifestsByXuid_entry(u32 user_index, u32 xuid_count, mapped_u64 xuid, u32 unk,
                                      u32 avatar_info_ptr, mapped_void overlapped_ptr) {
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(),
                                                    X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_STATUS_SUCCESS;
}

// Per-component CPU/GPU buffer sizes (avatars::ComponentCategory order).
namespace {
struct AvatarComponentAssetInfo {
  uint32_t cpu_size;
  uint32_t gpu_size;
};
const AvatarComponentAssetInfo kAvatarComponentAssetInfos[] = {
    {0x00000800, 0x00053800},  // kHead
    {0x00000400, 0x00012C00},  // kBody
    {0x00000800, 0x0000E000},  // kHair
    {0x00000800, 0x00014400},  // kTop
    {0x00000800, 0x0000F000},  // kBottom
    {0x00000800, 0x0000D400},  // kShoes
    {0x00000800, 0x0000A400},  // kHat
    {0x00000800, 0x00006800},  // kGloves
    {0x00000800, 0x00008400},  // kGlasses
    {0x00000800, 0x00005C00},  // kWristwear
    {0x00000800, 0x00005800},  // kEarrings
    {0x00000800, 0x00005C00},  // kRing
    {0x00000800, 0x00019000},  // kProp
};
}  // namespace

// ---------------------------------------------------------------------------
// Random avatar metadata (World Tour NPC/AI opponents).
//
// DCC2's AVATARMODELMANAGER::RetrieveMetadata (0x829DD0B0) fetches every
// non-local avatar via XamAvatarGetMetadataRandom (body mask 3, count 1); the
// signed-out-profile path also falls back to it since 0 signed-out profiles
// are reported. A zeroed manifest leaves the body component unresolved
// (invisible bodies with the game's gear models still attached), so synthesize
// a valid one: a title preset if any, else a stock male/female body + random
// pack assets for the visible wardrobe categories, with version/factors/colors
// taken from the player's metadata when available.
//
// Synthesized metadata carries kRandomAvatarXuid in owner_xuid so GetAssets
// can tell an NPC build from the player's; NPC builds never refresh the
// captured player manifest.
static constexpr uint64_t kRandomAvatarXuid = 0x4E50435241564154ull;  // "NPCRAVAT"

// ---------------------------------------------------------------------------
// DCC2 AI opponent looks. data/art/_avatar/ai/*.amd are 1000-byte
// X_AVATAR_METADATA manifests for the named World Tour opponents. Six of them
// (kAiLookFiles) are byte-identical theme bases paired with a special STRB
// wearable in data/art/_avatar/ai-assets/ (pairing declared in
// data/templates/avatarmodel.wbr): sgt peppers jacket, anubis mask, UFO,
// pirate gear, robot, voltron tee. On hardware that gear attaches through the
// title custom-asset path, which XamAvatarSetCustomAsset does not implement.
// Instead every non-player XamAvatarGetAssets build is dealt a look: the .amd
// becomes the build's metadata and the .bin is queued as an appended component
// (SetPendingExtraComponentStrb). Shuffled-deck dealing keeps the 3 AIs of a
// race distinct.
REXCVAR_DEFINE_BOOL(dcc2_ai_avatar_randomize, true, "Kernel",
                    "Give AI/NPC avatar builds a random look from the game's shipped AI "
                    "appearances (every data/art/_avatar/ai/*.amd manifest, ~32 named "
                    "avatars, plus the six special-costume looks paired with ai-assets STRB "
                    "gear) instead of the manifest the title supplies.");
REXCVAR_DEFINE_INT32(dcc2_ai_costume_weight, 2, "Kernel",
                     "Deck entries per special-costume AI look (sgt peppers, anubis, UFO, "
                     "pirate, robot, voltron tee); each plain named look gets one. Unweighted, "
                     "the costumes show 6-in-38 against the named roster. 0 hides them.")
    .range(0, 16);

namespace {
struct AiLookFiles {
  const char* name;
  const char* amd;  // under data/art/_avatar/ai/
  const char* bin;  // under data/art/_avatar/ai-assets/
};
constexpr AiLookFiles kAiLookFiles[] = {
    {"sgt_peppers", "arctic.amd", "sgt_peppers_asset.bin"},
    {"anubis", "desert.amd", "male_anubis.bin"},
    {"ufo", "jungle.amd", "male_ufo.bin"},
    {"pirate", "island.amd", "male_pirate.bin"},
    {"robot", "hongkong.amd", "robot_asset.bin"},
    {"voltron_tee", "joon.amd", "male_voltron_tee.bin"},
};

struct LoadedAiLook {
  std::string name;
  std::vector<uint8_t> amd;   // 1000-byte X_AVATAR_METADATA
  std::vector<uint8_t> strb;  // special-asset STRB (may be empty)
};

std::mutex g_ai_look_mutex;
std::vector<LoadedAiLook> g_ai_looks;  // loaded once, only valid entries
std::vector<size_t> g_ai_look_deck;     // shuffled indices (full roster deal)
std::vector<size_t> g_ai_costume_deck;  // shuffled indices of gear-carrying looks
bool g_ai_looks_load_attempted = false;
size_t g_ai_last_dealt = SIZE_MAX;
}  // namespace

// Load every AI look once. Caller holds g_ai_look_mutex.
static void EnsureAiLooksLoadedLocked() {
  if (g_ai_looks_load_attempted) {
    return;
  }
  g_ai_looks_load_attempted = true;
  {
    const std::string root = REXCVAR_GET(game_data_root);
    const std::filesystem::path base =
        (root.empty() ? std::filesystem::path(".") : std::filesystem::path(root)) / "data" /
        "art" / "_avatar";
    // Every *.amd in ai/ is a complete 1000-byte avatar manifest. Most are
    // distinct named World Tour opponents (acuna, akio, carter, ...); the six
    // theme-named ones (arctic/desert/jungle/island/hongkong/joon) are
    // byte-identical bases whose look comes from the paired ai-assets STRB
    // gear (kAiLookFiles). All are loaded so races draw from the full roster.
    std::vector<std::filesystem::path> amd_paths;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(base / "ai", ec), end; it != end;
         it.increment(ec)) {
      if (it->is_regular_file() && it->path().extension() == ".amd") {
        amd_paths.push_back(it->path());
      }
    }
    std::sort(amd_paths.begin(), amd_paths.end());  // deterministic load order
    for (const auto& amd_path : amd_paths) {
      const std::string amd_name = amd_path.filename().string();
      LoadedAiLook look;
      look.name = amd_path.stem().string();
      look.amd = ReadHostFile(amd_path.string());
      if (look.amd.size() != sizeof(avatars::X_AVATAR_METADATA)) {
        REXKRNL_WARN("[avatar] AI look '{}': bad manifest {} ({} bytes)", look.name, amd_name,
                     look.amd.size());
        continue;
      }
      for (const auto& files : kAiLookFiles) {
        if (amd_name == files.amd) {
          look.name = files.name;  // the costume's name, not the theme's
          look.strb = ReadHostFile((base / "ai-assets" / files.bin).string());
          if (look.strb.empty()) {
            REXKRNL_WARN("[avatar] AI look '{}': missing special asset {} (manifest only)",
                         look.name, files.bin);
          }
          break;
        }
      }
      g_ai_looks.push_back(std::move(look));
    }
    REXKRNL_INFO("[avatar] loaded {} AI looks ({} with costume gear) from {}", g_ai_looks.size(),
                 std::count_if(g_ai_looks.begin(), g_ai_looks.end(),
                               [](const LoadedAiLook& l) { return !l.strb.empty(); }),
                 base.string());
  }
}

// Deal the next index from a shuffled deck, avoiding an immediate repeat of
// g_ai_last_dealt (the weighted deck holds duplicate costume indices, which a
// shuffle can leave adjacent -- two NPCs of one race would twin otherwise).
// Caller holds g_ai_look_mutex and guarantees a non-empty refill.
static size_t DealFromDeckLocked(std::vector<size_t>& deck,
                                 const std::function<void(std::vector<size_t>&)>& refill) {
  static std::mt19937 rng{std::random_device{}()};
  if (deck.empty()) {
    refill(deck);
    std::shuffle(deck.begin(), deck.end(), rng);
  }
  size_t pick_at = deck.size() - 1;
  if (deck[pick_at] == g_ai_last_dealt) {
    for (size_t j = deck.size(); j-- > 0;) {
      if (deck[j] != g_ai_last_dealt) {
        pick_at = j;
        break;
      }
    }
  }
  const size_t idx = deck[pick_at];
  deck.erase(deck.begin() + static_cast<ptrdiff_t>(pick_at));
  g_ai_last_dealt = idx;
  return idx;
}

// Deal the next randomized AI look (shuffled deck, reshuffle on exhaustion,
// no immediate repeats). costume_only restricts the deal to the six looks
// carrying ai-assets STRB gear (used for theme-base clip ghosts, whose six
// shipped manifests are byte-identical -- the gear is the only identity, so
// cycle it). Returns nullptr when the asset files are missing. Caller holds
// no locks; returned pointer is stable (g_ai_looks never shrinks).
static const LoadedAiLook* DealAiLook(bool costume_only) {
  std::lock_guard<std::mutex> lock(g_ai_look_mutex);
  EnsureAiLooksLoadedLocked();
  if (g_ai_looks.empty()) {
    return nullptr;
  }
  if (costume_only) {
    bool have_costume = false;
    for (const auto& look : g_ai_looks) {
      if (!look.strb.empty()) {
        have_costume = true;
        break;
      }
    }
    if (have_costume) {
      const size_t idx = DealFromDeckLocked(g_ai_costume_deck, [](std::vector<size_t>& deck) {
        for (size_t i = 0; i < g_ai_looks.size(); ++i) {
          if (!g_ai_looks[i].strb.empty()) {
            deck.push_back(i);
          }
        }
      });
      return &g_ai_looks[idx];
    }
    // No gear on disk: fall through to a full-roster deal.
  }
  const size_t idx = DealFromDeckLocked(g_ai_look_deck, [](std::vector<size_t>& deck) {
    const int costume_weight = REXCVAR_GET(dcc2_ai_costume_weight);
    for (size_t i = 0; i < g_ai_looks.size(); ++i) {
      const int copies = g_ai_looks[i].strb.empty() ? 1 : costume_weight;
      for (int c = 0; c < copies; ++c) {
        deck.push_back(i);
      }
    }
    if (deck.empty()) {
      // costume_weight=0 with a costume-only load (no named amds on disk):
      // fall back to a fair deck rather than dealing nothing.
      for (size_t i = 0; i < g_ai_looks.size(); ++i) {
        deck.push_back(i);
      }
    }
  });
  return &g_ai_looks[idx];
}

// Identify a non-player GetAssets manifest against the shipped AI looks: the
// World Tour/Time Trial ghost clips (data/clips/<level>/<time>_<name>.clip)
// embed the recorded character's manifest verbatim, so a match identifies the
// build as a clip ghost and names the character. Returns nullptr for unknown
// manifests.
static const LoadedAiLook* MatchAiLookManifest(const avatars::X_AVATAR_METADATA* metadata) {
  std::lock_guard<std::mutex> lock(g_ai_look_mutex);
  EnsureAiLooksLoadedLocked();
  for (const auto& look : g_ai_looks) {
    if (look.amd.size() == sizeof(*metadata) &&
        !std::memcmp(look.amd.data(), metadata, sizeof(*metadata))) {
      return &look;
    }
  }
  return nullptr;
}

// The GUID `d` tail every constructed pack-asset id carries (matches the
// stock bodies and the ids inside the Avatar Editor's shipped presets).
static constexpr uint8_t kPackAssetGuidTail[8] = {0xC1, 0xC8, 0xF1, 0x09,
                                                  0xA1, 0x9C, 0xB2, 0xE0};

// Pick a random pack asset of `category` and produce a metadata AssetId that
// FindAsset (a pure index on AssetId.b) resolves back to it. The V2 pack holds
// two populations:
//  - legacy entries: `categories` is the raw bit and asset_ids[0] is a real,
//    self-indexed id, used verbatim;
//  - V2 entries: `categories == category << 16` and asset_ids are zero; the
//    id is constructed as {a=category<<16, b=index, c=bodies, d=tail}, the
//    encoding of the editor's shipped Avatar*.Avatar manifests. Heads exist
//    only in this population.
static bool PickRandomPackAsset(const avatars::AssetPack& pack, uint32_t category,
                                uint32_t body_bit, uint32_t& rng, avatars::AssetId* out_id) {
  auto next = [&rng]() {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
  };
  const auto& infos = pack.asset_infos();
  auto matches = [&](size_t i) {
    const auto& info = infos[i];
    const bool legacy_match = (info.categories & category) && !info.asset_ids[0].is_zero() &&
                              info.asset_ids[0].b.get() == i;
    const bool v2_match = info.categories == (category << 16);
    if (!legacy_match && !v2_match) return false;
    // Respect the pack's body suitability when it declares one.
    const uint8_t suit = info.random_bodies ? info.random_bodies : info.bodies;
    return !suit || (suit & body_bit) != 0;
  };
  uint32_t n_match = 0;
  for (size_t i = 0; i < infos.size(); ++i) {
    if (matches(i)) ++n_match;
  }
  if (!n_match) {
    return false;
  }
  uint32_t pick = next() % n_match;
  for (size_t i = 0; i < infos.size(); ++i) {
    if (matches(i) && !pick--) {
      const auto& info = infos[i];
      if (!info.asset_ids[0].is_zero() && info.asset_ids[0].b.get() == i) {
        *out_id = info.asset_ids[0];
      } else {
        *out_id = {};
        out_id->a = category << 16;
        out_id->b = static_cast<uint16_t>(i);
        // .c = the asset's gender/bodies mask (see the enumerator note).
        out_id->c = info.bodies ? info.bodies : 3;
        std::memcpy(out_id->d, kPackAssetGuidTail, sizeof(out_id->d));
      }
      return true;
    }
  }
  return false;
}

// Title-shipped complete avatar manifests (the Avatar Editor carries
// Avatar*.Avatar files -- each exactly 1000 bytes of X_AVATAR_METADATA with a
// full body+head+wardrobe authored by the title). When present these are the
// preferred source for "random" avatars: they load end-to-end, while pack-pick
// synthesis depends on loader validation rules that are only partly understood.
static const std::vector<std::vector<uint8_t>>& LoadPresetAvatarManifests() {
  static std::vector<std::vector<uint8_t>> presets;
  static bool attempted = false;
  if (!attempted) {
    attempted = true;
    const std::string root = REXCVAR_GET(game_data_root);
    if (!root.empty()) {
      // Recursive: titles ship preset manifests in different spots and
      // formats -- the Avatar Editor carries Avatar*.Avatar at the root,
      // Crash Course 1 carries the dev team's avatars as
      // data/art/_avatar/credits/*.amd. Each is exactly one X_AVATAR_METADATA.
      std::error_code ec;
      for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
           it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".avatar" && ext != ".amd") continue;
        auto data = ReadHostFile(it->path().string());
        if (data.size() == sizeof(avatars::X_AVATAR_METADATA)) {
          presets.push_back(std::move(data));
        }
      }
      if (!presets.empty()) {
        REXKRNL_INFO("[avatar] loaded {} preset avatar manifests from {}", presets.size(), root);
      }
    }
  }
  return presets;
}

static void BuildRandomAvatarMetadata(avatars::X_AVATAR_METADATA* out, uint32_t body_mask,
                                      uint32_t salt) {
  namespace cat = avatars::ComponentCategory;
  std::memset(out, 0, sizeof(*out));

  // Prefer a title-shipped preset manifest when one matches the requested
  // body type (GUID.c: 1 = male, 2 = female on the stock bodies).
  {
    const auto& presets = LoadPresetAvatarManifests();
    if (!presets.empty()) {
      uint32_t rng = salt * 2654435761u + 0x9E3779B9u;
      rng = rng * 1664525u + 1013904223u;
      for (size_t k = 0; k < presets.size(); ++k) {
        const auto& raw = presets[(rng + k) % presets.size()];
        const auto* meta = reinterpret_cast<const avatars::X_AVATAR_METADATA*>(raw.data());
        const uint16_t gender = meta->body_component.asset_id.c.get();
        if (!(body_mask & (gender == 1 ? 1u : 2u))) continue;
        std::memcpy(out, raw.data(), sizeof(*out));
        out->owner_xuid = kRandomAvatarXuid;
        return;
      }
    }
  }

  // Version, factors and colors come from the player's metadata when present;
  // otherwise defaults.
  if (g_have_metadata) {
    const auto* base = reinterpret_cast<const avatars::X_AVATAR_METADATA*>(g_captured_metadata);
    out->version = base->version;
    out->weight_factor = base->weight_factor;
    out->height_factor = base->height_factor;
    std::memcpy(out->colors, base->colors, sizeof(out->colors));
  } else {
    out->version = 1;
    out->weight_factor = 1.0f;
    out->height_factor = 1.0f;
    for (auto& c : out->colors) c = 0xFF808080u;
    out->colors[0] = 0xFFE8C098u;  // skin
    out->colors[1] = 0xFF54341Cu;  // hair
  }

  uint32_t rng = salt * 2654435761u + 0x9E3779B9u;
  auto next = [&rng]() {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
  };

  // Stock body: exactly the ids GetBodyType (guest_load_asset.cpp) recognizes.
  const bool male = (body_mask & 1u) && (!(body_mask & 2u) || (next() & 1u));
  const avatars::AssetId body_id = {2, 0, static_cast<uint16_t>(male ? 1 : 2),
                                    {0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0}};
  const uint32_t body_bit = male ? 1u : 2u;
  out->body_component.asset_id = body_id;
  out->body_component.categories = static_cast<uint16_t>(cat::kBody);

  avatars::AssetPack* pack =
      (g_title_version < 3) ? &g_legacy_avatar_asset_pack : &g_avatar_asset_pack;
  if (!pack->is_loaded()) {
    // Fall back to whichever pack did load (the Avatar Editor initializes with
    // a low version but ships only the V2 pack, and vice versa for old dumps).
    pack = pack == &g_avatar_asset_pack ? &g_legacy_avatar_asset_pack : &g_avatar_asset_pack;
  }
  if (pack->is_loaded()) {
    avatars::AssetId picked;
    if (PickRandomPackAsset(*pack, cat::kHead, body_bit, rng, &picked)) {
      out->head_component.asset_id = picked;
      out->head_component.categories = static_cast<uint16_t>(cat::kHead);
    }
    const struct {
      uint32_t category;
      uint32_t chance_pct;
    } kSlots[] = {{cat::kHair, 100}, {cat::kTop, 100}, {cat::kBottom, 100},
                  {cat::kShoes, 100}, {cat::kHat, 35},  {cat::kGlasses, 20}};
    uint32_t slot = 0;
    for (const auto& s : kSlots) {
      if (slot >= rex::countof(out->components)) {
        break;
      }
      if ((next() % 100u) >= s.chance_pct) {
        continue;
      }
      if (PickRandomPackAsset(*pack, s.category, body_bit, rng, &picked)) {
        out->components[slot].asset_id = picked;
        out->components[slot].categories = static_cast<uint16_t>(s.category);
        ++slot;
      }
    }
  }
  out->owner_xuid = kRandomAvatarXuid;
}

// Buffer sizes a build with `mask` needs: the table budgets stock
// categories, but a worn component covering categories outside the mask (a
// fused costume is top+bottom+shoes) is still emitted whole, so its other
// categories join the budget, and marketplace items get headroom the stock
// figures lack. The title places its own data right after these sizes;
// overrunning them puts the avatar's bone palette under the next build's
// bytes.
static void AssetBuildBudget(uint32_t mask, uint32_t* cpu_size, uint32_t* gpu_size) {
  uint32_t budget = mask & 0x1fffu;
  if (g_have_metadata) {
    const auto* meta =
        reinterpret_cast<const avatars::X_AVATAR_METADATA*>(g_captured_metadata);
    for (const auto& comp : meta->components) {
      const uint32_t cats = comp.categories;
      if (!comp.asset_id.is_zero() && (cats & mask)) {
        budget |= cats & 0x1fffu;
      }
    }
  }
  uint32_t cpu = !(mask & avatars::ComponentCategory::kProp) ? 0x1F80u : 0x3147Cu;
  uint32_t gpu = 0;
  for (size_t i = 0; i < rex::countof(kAvatarComponentAssetInfos); ++i) {
    if (budget & (1u << i)) {
      cpu += rex::align(kAvatarComponentAssetInfos[i].cpu_size, 16u);
      gpu += rex::align(kAvatarComponentAssetInfos[i].gpu_size, 4096u);
    }
  }
  // A hatted avatar carries its full hair beside the flattened hat hair (LoadAssetsToGuest).
  constexpr uint32_t kHatAndHair = avatars::ComponentCategory::kHat | avatars::ComponentCategory::kHair;
  if ((budget & kHatAndHair) == kHatAndHair) {
    cpu += rex::align(kAvatarComponentAssetInfos[2].cpu_size, 16u);
    gpu += rex::align(kAvatarComponentAssetInfos[2].gpu_size, 4096u);
  }
  cpu += 0x1000u;
  gpu += 0x10000u;
  // Headroom for the randomized AI look's appended special-asset component
  // (ai-assets STRB gear, decoded mesh + textures). The MemoryBlocks are not
  // bounds-checked, so the extra component must be budgeted here.
  if (REXCVAR_GET(dcc2_ai_avatar_randomize)) {
    cpu += 0x40000u;    // 256 KiB CPU (vertex/index/struct data)
    gpu += 0x200000u;   // 2 MiB GPU (textures)
  }
  *cpu_size = cpu;
  *gpu_size = gpu;
}

u32 XamAvatarGetAssetsResultSize_entry(u32 avatar_component_mask, mapped_u32 result_buffer_size_ptr,
                                       mapped_u32 gpu_resource_buffer_size_ptr) {
  uint32_t cpu_size = 0, gpu_size = 0;
  AssetBuildBudget(avatar_component_mask, &cpu_size, &gpu_size);
  REXKRNL_DEBUG("[avatar] GetAssetsResultSize(mask={:#x}) -> cpu={:#x} gpu={:#x}",
               avatar_component_mask, cpu_size, gpu_size);
  if (result_buffer_size_ptr) *result_buffer_size_ptr = cpu_size;
  if (gpu_resource_buffer_size_ptr) *gpu_resource_buffer_size_ptr = gpu_size;
  return X_STATUS_SUCCESS;
}

u32 XamAvatarGetAssets_entry(ppc_ptr_t<X_AVATAR_METADATA> avatar_metadata_ptr,
                             u32 avatar_component_mask, u32 flags, mapped_u32 result_buffer_ptr,
                             mapped_u32 gpu_resource_buffer_ptr, mapped_void overlapped_ptr) {
  const uint32_t meta_addr = avatar_metadata_ptr.guest_address();
  const uint32_t cpu_addr = result_buffer_ptr.guest_address();
  const uint32_t gpu_addr = gpu_resource_buffer_ptr.guest_address();

  auto run = [=]() -> X_RESULT {
    auto* mem = REX_KERNEL_MEMORY();
    if (!meta_addr || !cpu_addr || !gpu_addr) {
      return X_ERROR_FUNCTION_FAILED;
    }
    avatars::AssetPack* pack = &g_avatar_asset_pack;
    uint32_t skeleton_version = 2;
    if (g_title_version < 3) {
      pack = &g_legacy_avatar_asset_pack;
      skeleton_version = 1;
    }
    if (!pack->is_loaded() && !(pack == &g_avatar_asset_pack && LoadAvatarAssetPack())) {
      REXKRNL_WARN("[avatar] GetAssets: asset pack not loaded; cannot build avatar");
      return X_ERROR_FUNCTION_FAILED;
    }
    const auto* metadata =
        mem->TranslateVirtual<const avatars::X_AVATAR_METADATA*>(meta_addr);
    // Distinguish the local player's build from AI/NPC builds. The player's
    // manifest is byte-identical to the captured one (GetManifestLocalUser
    // feeds the game that exact buffer); AI manifests come from the title's
    // data/art/_avatar/ai/*.amd files, and XamAvatarGetMetadataRandom output
    // carries kRandomAvatarXuid. Only the player's build refreshes the
    // captured manifest.
    const bool random_npc =
        static_cast<uint64_t>(metadata->owner_xuid) == kRandomAvatarXuid;
    // Lobby members' manifests come stamped with their own XUID (cc2::lobby), and must render as sent.
    const bool remote_player =
        !random_npc && cc2::lobby::IsPeerXuid(static_cast<uint64_t>(metadata->owner_xuid));
    uint32_t local_player = 0;  // 2-4: a local player's build, dealt a dev avatar like the AI
    for (uint32_t u = 1; u < 4; ++u) {
      if (static_cast<uint64_t>(metadata->owner_xuid) == cc2::kernel::xam::LocalPlayerXuid(u)) local_player = u + 1;
    }
    const bool is_player_build =
        !random_npc && !remote_player && !local_player &&
        (!g_have_metadata ||
         std::memcmp(metadata, g_captured_metadata, sizeof(g_captured_metadata)) == 0);
    if (is_player_build) {
      // Capture this (valid) avatar manifest to replay from GetManifestLocalUser.
      std::memcpy(g_captured_metadata, metadata, sizeof(g_captured_metadata));
      if (!g_have_metadata) {
        // Persist it so the next launch has valid metadata before the master
        // loads; this launch captures it too late to feed the master.
        WriteHostFile(AvatarManifestPath(), g_captured_metadata, sizeof(g_captured_metadata));
        REXKRNL_INFO("[avatar] cached avatar metadata to {}", AvatarManifestPath());
      }
      g_have_metadata = true;
      // Copy it into the buffer GetManifestLocalUser last filled (DCC2's
      // manager metadata buffer) so the "Avatar metadata not set" retry
      // succeeds when the manifest read preceded GetAssets.
      if (g_last_manifest_dest) {
        std::memcpy(mem->TranslateVirtual<uint8_t*>(g_last_manifest_dest), metadata,
                    sizeof(g_captured_metadata));
      }
    }
    // Non-player build: deal a random look from the full roster deck (~32
    // named World Tour characters + the six costume looks, costume-weighted)
    // in place of the canonical per-track ghosts the clips embed. The incoming
    // manifest is matched against the shipped AI looks only for the log.
    avatars::X_AVATAR_METADATA ai_metadata;
    if (remote_player) {
      REXKRNL_INFO("[avatar] remote player build ({:016X})",
                   static_cast<uint64_t>(metadata->owner_xuid));
    } else if (local_player && LocalPlayerManifest(local_player - 1, nullptr)) {
      REXKRNL_INFO("[avatar] local player build (player {}) -> the manifest in Avatar/Player{}", local_player,
                   local_player);
    } else if (!is_player_build && REXCVAR_GET(dcc2_ai_avatar_randomize)) {
      if (const LoadedAiLook* look = DealAiLook(/*costume_only=*/false)) {
        const LoadedAiLook* matched = random_npc ? nullptr : MatchAiLookManifest(metadata);
        std::memcpy(&ai_metadata, look->amd.data(), sizeof(ai_metadata));
        metadata = &ai_metadata;
        if (!look->strb.empty()) {
          avatars::SetPendingExtraComponentStrb(look->strb.data(), look->strb.size());
        }
        REXKRNL_INFO("[avatar] {} build ({}) -> randomized look '{}'", local_player ? "local player" : "AI",
                     local_player        ? fmt::format("player {}", local_player)
                     : random_npc        ? std::string("random NPC")
                     : matched           ? fmt::format("clip ghost '{}'", matched->name)
                                         : std::string("unknown manifest"),
                     look->name);
      }
    }
    auto* cpu_host = mem->TranslateVirtual<uint8_t*>(cpu_addr);
    auto* gpu_host = mem->TranslateVirtual<uint8_t*>(gpu_addr);
    // The title recycles a small set of GPU resource buffers across avatar
    // builds (the Avatar Editor ping-pongs two, one GetAssets call per
    // selection-grid tile). The command processor runs deferred, so this
    // rewrite can land while texture loads for the previous build at the same
    // addresses are still pending (garbled face decals, DXT noise). On
    // hardware the fetch is in-order within the frame and the title's own
    // fencing suffices.
    avatars::MemoryBlock cpu_memory(cpu_host, 16);
    avatars::MemoryBlock gpu_memory(gpu_host, 4096);
    bool ok = avatars::LoadAssetsToGuest(*metadata, avatar_component_mask, flags, pack,
                                         &cpu_memory, &gpu_memory, skeleton_version,
                                         g_coordinate_system);
    avatars::SetPendingExtraComponentStrb(nullptr, 0);  // clear leftover on failure
    if (!ok) {
      REXKRNL_WARN("[avatar] GetAssets: LoadAssetsToGuest failed (mask={:#x})",
                   avatar_component_mask);
      return X_ERROR_FUNCTION_FAILED;
    }
    cpu_memory.ResolvePointers(cpu_addr);
    gpu_memory.ResolvePointers(gpu_addr);
    {
      // Past the budget means the title's neighbouring data is already gone.
      uint32_t budget_cpu = 0, budget_gpu = 0;
      AssetBuildBudget(avatar_component_mask, &budget_cpu, &budget_gpu);
      if (cpu_memory.used() > budget_cpu || gpu_memory.used() > budget_gpu) {
        REXKRNL_WARN("[avatar] GetAssets: build (mask={:#x}) used cpu {:#x} gpu {:#x}, over the "
                     "{:#x}/{:#x} the title was told to allocate",
                     avatar_component_mask, cpu_memory.used(), gpu_memory.used(), budget_cpu,
                     budget_gpu);
      }
    }
    return X_ERROR_SUCCESS;
  };

  if (overlapped_ptr) {
    X_RESULT result = run();
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  }
  return run();
}

u32 XamAvatarSetCustomAsset_entry(u32 buffer_size, mapped_u32 asset_data_ptr,
                                  u32 custom_color_count, mapped_u32 custom_colors_ptr,
                                  ppc_ptr_t<X_AVATAR_METADATA> avatar_metadata_ptr) {
  return X_STATUS_SUCCESS;
}

u32 XamAvatarSetManifest_entry(u32 user_index, u32 avatar_info_ptr, mapped_void overlapped_ptr) {
  // Persist the manifest being saved (the Avatar Editor's "Save and Exit")
  // into the same store GetManifestLocalUser replays from, so the edited
  // avatar becomes the local user's avatar on every later launch.
  if (avatar_info_ptr) {
    const auto* meta =
        REX_KERNEL_MEMORY()->TranslateVirtual<const avatars::X_AVATAR_METADATA*>(avatar_info_ptr);
    if (!meta->body_component.asset_id.is_zero()) {
      std::memcpy(g_captured_metadata, meta, sizeof(g_captured_metadata));
      g_have_metadata = true;
      WriteHostFile(AvatarManifestPath(), g_captured_metadata, sizeof(g_captured_metadata));
      REXKRNL_INFO("[avatar] XamAvatarSetManifest(user={}): persisted manifest to {}", user_index,
                   AvatarManifestPath());
    } else {
      REXKRNL_WARN(
          "[avatar] XamAvatarSetManifest: buffer at {:#x} has a zero body component; "
          "not persisting (unexpected layout?)",
          avatar_info_ptr);
    }
  }
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(),
                                                    X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_STATUS_SUCCESS;
}

u32 XamAvatarGetMetadataRandom_entry(u32 body_type, u32 avatars_count,
                                     ppc_ptr_t<X_AVATAR_METADATA> avatar_metadata_ptr,
                                     mapped_void overlapped_ptr) {
  static uint32_t call_no = 0;
  auto run = [&]() -> X_RESULT {
    if (!avatar_metadata_ptr || !avatars_count) {
      return X_ERROR_INVALID_PARAMETER;
    }
    auto* out = REX_KERNEL_MEMORY()->TranslateVirtual<avatars::X_AVATAR_METADATA*>(
        avatar_metadata_ptr.guest_address());
    const uint32_t mask = (body_type & 3u) ? (body_type & 3u) : 3u;
    for (uint32_t i = 0; i < avatars_count; ++i) {
      BuildRandomAvatarMetadata(&out[i], mask, ++call_no * 97u + i);
    }
    REXKRNL_DEBUG(
        "[avatar] XamAvatarGetMetadataRandom(body={:#x}, count={}) -> synthesized (body={}, "
        "head={}, comp0={})",
        body_type, avatars_count, out[0].body_component.asset_id.to_string(),
        out[0].head_component.asset_id.to_string(), out[0].components[0].asset_id.to_string());
    return X_ERROR_SUCCESS;
  };
  if (overlapped_ptr) {
    const X_RESULT result = run();
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  }
  return run();
}

u32 XamAvatarGetMetadataSignedOutProfileCount_entry(mapped_u32 profile_count_ptr,
                                                    mapped_void overlapped_ptr) {
  REXKRNL_DEBUG("[avatar] XamAvatarGetMetadataSignedOutProfileCount");
  if (profile_count_ptr) *profile_count_ptr = 0;
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(),
                                                    X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_STATUS_SUCCESS;
}

u32 XamAvatarGetMetadataSignedOutProfile_entry(u32 profile_index,
                                               ppc_ptr_t<X_AVATAR_METADATA> avatar_metadata_ptr,
                                               mapped_void overlapped_ptr) {
  REXKRNL_DEBUG("[avatar] XamAvatarGetMetadataSignedOutProfile(idx={})", profile_index);
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(),
                                                    X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_STATUS_SUCCESS;
}

u32 XamAvatarManifestGetBodyType_entry(ppc_ptr_t<X_AVATAR_METADATA> avatar_metadata_ptr) {
  REXKRNL_DEBUG("[avatar] XamAvatarManifestGetBodyType");
  return static_cast<u32>(X_AVATAR_BODY_TYPE::Male);
}

u32 XamAvatarGetInstrumentation_entry(u64 unk1, mapped_u32 unk2) {
  if (unk2) *unk2 = 0;
  return 1;
}

u32 XamAvatarGetAssetIcon_entry(mapped_u64 unk1, u32 unk2, mapped_u64 unk3, mapped_u64 unk4,
                                mapped_void overlapped_ptr) {
  // Report failure like xenia: claiming success without writing pixels makes
  // the editor draw uninitialized icon buffers; on failure it falls back to
  // rendering the asset itself.
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(),
                                                    X_ERROR_FUNCTION_FAILED);
    return X_ERROR_IO_PENDING;
  }
  return X_E_FAIL;
}

// XamAvatarGetAssetBinary(id, flags, size_inout, buffer, overlapped): copies
// the asset's raw STRB bytes (pack or closet) to the title. The Avatar Editor
// fetches every selection-grid item through this (2MB buffer, in/out byte
// count, see sub_920B5990) and parses the STRB itself for the option preview.
u32 XamAvatarGetAssetBinary_entry(mapped_void asset_id_ptr, u32 flags, mapped_u32 size_inout,
                                  mapped_void buffer, mapped_void overlapped_ptr) {
  auto run = [&]() -> X_RESULT {
    if (!asset_id_ptr || !size_inout || !buffer) {
      return X_ERROR_INVALID_PARAMETER;
    }
    avatars::AssetPack* pack =
        (g_title_version < 3) ? &g_legacy_avatar_asset_pack : &g_avatar_asset_pack;
    if (!pack->is_loaded()) {
      LoadAvatarAssetPack();
    }
    if (!pack->is_loaded()) {
      pack = pack == &g_avatar_asset_pack ? &g_legacy_avatar_asset_pack : &g_avatar_asset_pack;
    }
    if (!pack->is_loaded()) {
      return X_ERROR_FUNCTION_FAILED;
    }
    auto* mem = REX_KERNEL_MEMORY();
    const auto* id =
        mem->TranslateVirtual<const avatars::AssetId*>(asset_id_ptr.guest_address());
    const uint8_t* data = nullptr;
    size_t size = 0;
    std::vector<uint8_t> closet_bytes;
    if (avatars::GetCloset().ReadItemBytes(*id, closet_bytes) && !closet_bytes.empty()) {
      // Imported marketplace item: hand the title the raw YTGR/STRB blob.
      data = closet_bytes.data();
      size = closet_bytes.size();
    } else if (!pack->GetAssetData(*id, data, size) || !size) {
      REXKRNL_WARN("[avatar] GetAssetBinary: no data for {}", id->to_string());
      return X_ERROR_FUNCTION_FAILED;
    }
    const uint32_t capacity = *size_inout;
    if (size > capacity) {
      REXKRNL_WARN("[avatar] GetAssetBinary: {} needs {:#x} > capacity {:#x}", id->to_string(),
                   size, capacity);
      return X_ERROR_INSUFFICIENT_BUFFER;
    }
    std::memcpy(mem->TranslateVirtual<uint8_t*>(buffer.guest_address()), data, size);
    *size_inout = static_cast<uint32_t>(size);
    REXKRNL_DEBUG("[avatar] GetAssetBinary: {} -> {:#x} bytes", id->to_string(), size);
    return X_ERROR_SUCCESS;
  };
  if (overlapped_ptr) {
    const X_RESULT result = run();
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  }
  return run();
}

void XamAvatarGetInstalledAssetPackageDescription_entry(ppc_ptr_t<X_ASSET_ID> asset_id_ptr,
                                                        mapped_void content_data_ptr) {
  // rexglue ships no installed avatar-asset packages; report "none" by leaving
  // the descriptor as-is. (xenia fills an XCONTENT_AGGREGATE_DATA from the id.)
  REXKRNL_DEBUG("XamAvatarGetInstalledAssetPackageDescription (no installed packages)");
}

void XamAvatarSetMocks_entry() {
  // No-op.
}

// Animation ------------------------------------------------------------------
static const std::map<uint64_t, std::string> XAnimationTypeMap = {
    {0x0040000000030003, "Animation Generic Stand 0"},
    {0x0040000000040003, "Animation Generic Stand 1"},
    {0x0040000000050003, "Animation Generic Stand 2"},
    {0x0040000000270003, "Animation Generic Stand 3"},
    {0x0040000000280003, "Animation Generic Stand 4"},
    {0x0040000000290003, "Animation Generic Stand 5"},
    {0x00400000002A0003, "Animation Generic Stand 6"},
    {0x00400000002B0003, "Animation Generic Stand 7"},
    {0x0040000000130001, "Animation Male Idle Looks Around"},
    {0x0040000000140001, "Animation Male Idle Stretch"},
    {0x0040000000150001, "Animation Male Idle Shifts Weight"},
    {0x0040000000260001, "Animation Male Idle Checks Hand"},
    {0x0040000000090002, "Animation Female Idle Check Nails"},
    {0x00400000000A0002, "Animation Female Idle Looks Around"},
    {0x00400000000B0002, "Animation Female Idle Shifts Weight"},
    {0x00400000000C0002, "Animation Female Idle Fixes Shoe"},
};

u32 XamAvatarLoadAnimation_entry(mapped_u64 asset_id_ptr, u32 flags, mapped_void output,
                                 mapped_void overlapped_ptr) {
  if (asset_id_ptr) {
    uint64_t asset_id = *asset_id_ptr;
    auto it = XAnimationTypeMap.find(asset_id);
    if (it != XAnimationTypeMap.cend()) {
      REXKRNL_DEBUG("[avatar] LoadAnimation: {}", it->second);
    } else {
      REXKRNL_DEBUG("[avatar] LoadAnimation: Unknown 0x{:016x}", asset_id);
    }
  }

  const uint32_t asset_id_addr = asset_id_ptr.guest_address();
  const uint32_t output_addr = output.guest_address();

  auto run = [=]() -> X_RESULT {
    auto* mem = REX_KERNEL_MEMORY();
    if (!asset_id_addr || !output_addr) {
      return X_E_FAIL;
    }
    avatars::AssetPack* pack =
        (g_title_version >= 3) ? &g_avatar_asset_pack : &g_legacy_avatar_asset_pack;
    if (!pack->is_loaded()) {
      return X_E_FAIL;
    }
    const auto* asset_id = mem->TranslateVirtual<const avatars::AssetId*>(asset_id_addr);
    auto* animation = mem->TranslateVirtual<avatars::X_AVATAR_ANIMATION*>(output_addr);
    auto* compressed = mem->TranslateVirtual<uint8_t*>(animation->compressed_data_buffer_ptr);
    if (!avatars::LoadAnimationToGuest(pack, *asset_id, animation, compressed,
                                       g_coordinate_system)) {
      REXKRNL_WARN("[avatar] LoadAnimation failed");
      return X_E_FAIL;
    }
    return X_ERROR_SUCCESS;
  };

  if (overlapped_ptr) {
    X_RESULT result = run();
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  }
  return run();
}

u32 XamAvatarGenerateMipMaps_entry(mapped_u32 avatar_assets_ptr, u32 flags, u32 buffer_size,
                                   mapped_u32 mip_map_buffer_ptr, mapped_void overlapped_ptr) {
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(),
                                                    X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_STATUS_SUCCESS;
}

// Enum -----------------------------------------------------------------------
// Ported from xenia-canary avatars (xam_avatar_enum.cc). The Avatar Editor's
// feature-selection grids ("Change My Features/Style") are populated entirely
// through this enumerator; an empty enumeration also crashes selection
// (find-index over a null collection).
struct X_AVATAR_ASSET_INFO {
  avatars::AssetId asset_id;            // 0x000
  rex::be<uint32_t> categories;         // 0x010
  uint8_t body_mask;                    // 0x014
  uint8_t random_body_mask;             // 0x015
  rex::be<uint32_t> flags;              // 0x018
  uint8_t unknown[164];                 // 0x01C
  rex::be<uint32_t> subcategory;        // 0x0C0
  avatars::AssetId associated_ids[2];   // 0x0C4
  rex::be<uint16_t> name[64];           // 0x0E4
  rex::be<uint32_t> data_offset;        // 0x164
  rex::be<uint32_t> data_size;          // 0x168
  rex::be<uint16_t> description[100];   // 0x16C
  rex::be<uint32_t> title_id;           // 0x234
  rex::be<uint16_t> title_name[64];     // 0x238
  rex::be<uint32_t> time_0;             // 0x2B8
  rex::be<uint32_t> time_1;             // 0x2BC
  uint8_t skeleton_version_mask;        // 0x2C0
};
static_assert(sizeof(X_AVATAR_ASSET_INFO) == 0x2C4, "X_AVATAR_ASSET_INFO size");

namespace {
class AssetEnumerator {
 public:
  explicit AssetEnumerator(uint32_t items_per_enumerate = 50)
      : items_per_enumerate_(items_per_enumerate) {}

  size_t item_size() const { return sizeof(X_AVATAR_ASSET_INFO); }
  size_t items_per_enumerate() const { return items_per_enumerate_; }
  size_t item_count() const { return item_count_; }

  X_AVATAR_ASSET_INFO* AppendItem() {
    size_t offset = buffer_.size();
    buffer_.resize(offset + item_size());
    item_count_++;
    return reinterpret_cast<X_AVATAR_ASSET_INFO*>(&buffer_.data()[offset]);
  }

  uint32_t WriteItems(uint8_t* buffer_data, uint32_t* written_count) {
    size_t count = std::min(item_count_ - current_item_, items_per_enumerate());
    if (!count) {
      return X_ERROR_NO_MORE_FILES;
    }
    size_t size = count * item_size();
    size_t offset = current_item_ * item_size();
    std::memcpy(buffer_data, buffer_.data() + offset, size);
    current_item_ += count;
    if (written_count) {
      *written_count = static_cast<uint32_t>(count);
    }
    return X_ERROR_SUCCESS;
  }

 private:
  size_t items_per_enumerate_;
  size_t item_count_ = 0;
  size_t current_item_ = 0;
  std::vector<uint8_t> buffer_;
};

std::mutex g_asset_enum_mutex;
AssetEnumerator g_asset_enumerator(50);
}  // namespace

u32 XamAvatarBeginEnumAssets_entry(u32 user_index, u32 items_per_enumerate, u32 category_mask,
                                   u32 body_mask, u32 flags, mapped_void overlapped_ptr) {
  std::lock_guard<std::mutex> lock(g_asset_enum_mutex);
  g_asset_enumerator = AssetEnumerator(items_per_enumerate ? items_per_enumerate : 50);

  avatars::AssetPack* pack =
      (g_title_version < 3) ? &g_legacy_avatar_asset_pack : &g_avatar_asset_pack;
  if (!pack->is_loaded() && !LoadAvatarAssetPack()) {
    pack = nullptr;
  } else if (!pack->is_loaded()) {
    pack = pack == &g_avatar_asset_pack ? &g_legacy_avatar_asset_pack : &g_avatar_asset_pack;
  }
  if (pack && pack->is_loaded()) {
    const auto& infos = pack->asset_infos();
    // Paired wearables ship as catalog + companion entries: the catalog
    // entry's asset_ids[0].b points at its partner (the flattened
    // "worn-under-a-hat" variant, whose name ends in "(Hat)"). Enumerate only
    // the catalog halves; an entry pointed at by another entry's id is a
    // companion and would duplicate every style in the grid.
    std::vector<bool> is_companion(infos.size(), false);
    for (size_t index = 0; index < infos.size(); ++index) {
      const auto& id0 = infos[index].asset_ids[0];
      const size_t partner = id0.b;
      if (!id0.is_zero() && partner != index && partner < infos.size()) {
        is_companion[partner] = true;
      }
    }
    for (size_t index = 0; index < infos.size(); ++index) {
      const auto& asset_info = infos[index];
      if (!(asset_info.categories & category_mask) || !(asset_info.bodies & body_mask)) {
        continue;
      }
      if (is_companion[index]) {
        continue;
      }
      // The loader resolves every component id through AssetId.b (= pack
      // index, see FindAsset / GetAssetData), so the id handed out must
      // resolve to this entry. Most pack entries carry a zero asset_ids[0],
      // and catalog halves of paired wearables carry an id whose .b points at
      // their companion (the flattened "(Hat)" variant). Construct the id the
      // way the title's shipped preset manifests encode theirs:
      // {a = category bits, b = pack index, c = bodies mask, d = stock tail}.
      // .c is the asset's gender mask (1=male, 2=female, 3=unisex), not a
      // constant: the editor writes .c=bodies into saved-outfit manifests and
      // the outfit-display gate (sub_920B9898) requires an exact id match
      // against these entries, so a wrong .c hides any outfit containing a
      // gendered item from "My Outfits".
      avatars::AssetId id = asset_info.asset_ids[0];
      if (id.is_zero() || id.b.get() != static_cast<uint16_t>(index)) {
        id.a = asset_info.categories;
        id.b = static_cast<uint16_t>(index);
        id.c = asset_info.bodies ? asset_info.bodies : 3;
        std::memcpy(id.d, kPackAssetGuidTail, sizeof(id.d));
      }
      auto* guest_info = g_asset_enumerator.AppendItem();
      *guest_info = {};
      guest_info->asset_id = id;
      guest_info->categories = asset_info.categories;
      guest_info->body_mask = asset_info.bodies;
      guest_info->random_body_mask = asset_info.random_bodies;
      guest_info->flags = asset_info.flags;
      guest_info->subcategory = asset_info.subcategory;
      guest_info->data_offset = static_cast<uint32_t>(asset_info.data_offset);
      guest_info->data_size = static_cast<uint32_t>(asset_info.data_size);
      auto name = pack->GetAssetNameByIndex(index);
      const size_t n = std::min(name.size(), rex::countof(guest_info->name) - 1);
      for (size_t k = 0; k < n; ++k) {
        guest_info->name[k] = static_cast<uint16_t>(name[k]);
      }
    }
    // Imported marketplace items (the closet) appear in the same grids.
    // Their ids are full product GUIDs, resolved by GetAssetBinary /
    // LoadAsset through the closet rather than by pack index.
    for (const auto& item : avatars::GetCloset().items()) {
      if (!(item.categories & category_mask) || !(item.bodies & body_mask)) {
        continue;
      }
      auto* guest_info = g_asset_enumerator.AppendItem();
      *guest_info = {};
      guest_info->asset_id = item.id;
      guest_info->categories = item.categories;
      guest_info->body_mask = item.bodies;
      guest_info->random_body_mask = item.bodies;
      // Stock pack entries carry flags=0x01 and the editor enumerates with
      // flags=0x1 -- items without it are filtered out of the grids.
      guest_info->flags = 1;
      guest_info->subcategory = 0;
      const size_t n = std::min(item.name.size(), rex::countof(guest_info->name) - 1);
      for (size_t k = 0; k < n; ++k) {
        guest_info->name[k] = static_cast<uint16_t>(static_cast<uint8_t>(item.name[k]));
      }
    }
  }
  REXKRNL_INFO(
      "[avatar] BeginEnumAssets(user={}, per_enum={}, cat={:#x}, body={:#x}, flags={:#x}) -> {} "
      "assets",
      user_index, items_per_enumerate, category_mask, body_mask, flags,
      g_asset_enumerator.item_count());
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(),
                                                    X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_STATUS_SUCCESS;
}

u32 XamAvatarEnumAssets_entry(mapped_void buffer, mapped_u32 written_items, u32 unk3) {
  std::lock_guard<std::mutex> lock(g_asset_enum_mutex);
  if (!buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* out = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(buffer.guest_address());
  uint32_t written = 0;
  const uint32_t result = g_asset_enumerator.WriteItems(out, &written);
  if (written_items) {
    *written_items = written;
  }
  REXKRNL_DEBUG("[avatar] EnumAssets -> {} items ({})", written, result);
  return result;
}

u32 XamAvatarEndEnumAssets_entry(mapped_void overlapped_ptr) {
  {
    std::lock_guard<std::mutex> lock(g_asset_enum_mutex);
    g_asset_enumerator = AssetEnumerator(50);
  }
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(),
                                                    X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_STATUS_SUCCESS;
}

// Other ----------------------------------------------------------------------
u32 XamAvatarWearNow_entry(u64 unk1, mapped_u32 unk2, mapped_void overlapped_ptr) {
  X_RESULT result = X_ERROR_SUCCESS;
  if (REX_KERNEL_STATE()->title_id() != kAvatarEditorID) {
    if (overlapped_ptr) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
      return X_ERROR_IO_PENDING;
    }
  }
  return result;
}

u32 XamAvatarReinstallAwardedAsset_entry(mapped_string string_out_ptr, u32 string_size,
                                         mapped_u32 unk_ptr) {
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamAvatarInitialize, rex::kernel::xam::XamAvatarInitialize_entry)
REX_EXPORT(__imp__XamAvatarShutdown, rex::kernel::xam::XamAvatarShutdown_entry)
REX_EXPORT(__imp__XamAvatarGetManifestLocalUser,
           rex::kernel::xam::XamAvatarGetManifestLocalUser_entry)
REX_EXPORT(__imp__XamAvatarGetManifestsByXuid, rex::kernel::xam::XamAvatarGetManifestsByXuid_entry)
REX_EXPORT(__imp__XamAvatarGetAssetsResultSize,
           rex::kernel::xam::XamAvatarGetAssetsResultSize_entry)
REX_EXPORT(__imp__XamAvatarGetAssets, rex::kernel::xam::XamAvatarGetAssets_entry)
REX_EXPORT(__imp__XamAvatarSetCustomAsset, rex::kernel::xam::XamAvatarSetCustomAsset_entry)
REX_EXPORT(__imp__XamAvatarSetManifest, rex::kernel::xam::XamAvatarSetManifest_entry)
REX_EXPORT(__imp__XamAvatarGetMetadataRandom, rex::kernel::xam::XamAvatarGetMetadataRandom_entry)
REX_EXPORT(__imp__XamAvatarGetMetadataSignedOutProfileCount,
           rex::kernel::xam::XamAvatarGetMetadataSignedOutProfileCount_entry)
REX_EXPORT(__imp__XamAvatarGetMetadataSignedOutProfile,
           rex::kernel::xam::XamAvatarGetMetadataSignedOutProfile_entry)
REX_EXPORT(__imp__XamAvatarManifestGetBodyType,
           rex::kernel::xam::XamAvatarManifestGetBodyType_entry)
REX_EXPORT(__imp__XamAvatarGetInstrumentation,
           rex::kernel::xam::XamAvatarGetInstrumentation_entry)
REX_EXPORT(__imp__XamAvatarGetAssetIcon, rex::kernel::xam::XamAvatarGetAssetIcon_entry)
REX_EXPORT(__imp__XamAvatarGetAssetBinary, rex::kernel::xam::XamAvatarGetAssetBinary_entry)
REX_EXPORT(__imp__XamAvatarGetInstalledAssetPackageDescription,
           rex::kernel::xam::XamAvatarGetInstalledAssetPackageDescription_entry)
REX_EXPORT(__imp__XamAvatarSetMocks, rex::kernel::xam::XamAvatarSetMocks_entry)
REX_EXPORT(__imp__XamAvatarLoadAnimation, rex::kernel::xam::XamAvatarLoadAnimation_entry)
REX_EXPORT(__imp__XamAvatarGenerateMipMaps, rex::kernel::xam::XamAvatarGenerateMipMaps_entry)
REX_EXPORT(__imp__XamAvatarBeginEnumAssets, rex::kernel::xam::XamAvatarBeginEnumAssets_entry)
REX_EXPORT(__imp__XamAvatarEnumAssets, rex::kernel::xam::XamAvatarEnumAssets_entry)
REX_EXPORT(__imp__XamAvatarEndEnumAssets, rex::kernel::xam::XamAvatarEndEnumAssets_entry)
REX_EXPORT(__imp__XamAvatarWearNow, rex::kernel::xam::XamAvatarWearNow_entry)
REX_EXPORT(__imp__XamAvatarReinstallAwardedAsset,
           rex::kernel::xam::XamAvatarReinstallAwardedAsset_entry)

namespace cc2::kernel::xam {

void EnsureAvatarProfileSetting(rex::system::xam::UserProfile* profile) {
  rex::kernel::xam::EnsureAvatarProfileSettingImpl(profile);
}

bool LocalAvatarManifest(uint8_t* out) {
  namespace k = rex::kernel::xam;
  if (k::g_have_metadata) {
    std::memcpy(out, k::g_captured_metadata, sizeof(k::g_captured_metadata));
    return true;
  }
  const auto manifest = k::ReadPersistedManifest(nullptr);
  if (manifest.size() != sizeof(k::g_captured_metadata)) return false;
  std::memcpy(out, manifest.data(), manifest.size());
  return true;
}

bool LocalPlayerAvatarManifest(uint32_t user_index, uint8_t* out) {
  namespace k = rex::kernel::xam;
  if (user_index >= 4) return false;
  auto* manifest = reinterpret_cast<rex::avatars::X_AVATAR_METADATA*>(out);
  if (user_index && k::LocalPlayerManifest(user_index, manifest)) {
    manifest->owner_xuid = LocalPlayerXuid(user_index);
    return true;
  }
  if (!LocalAvatarManifest(out)) return false;
  if (user_index) manifest->owner_xuid = LocalPlayerXuid(user_index);
  return true;
}

}  // namespace cc2::kernel::xam
