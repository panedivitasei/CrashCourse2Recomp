/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include "cc2/lobby.h"
#include "cc2/online.h"
#include "cc2/kernel/xam/local_players.h"
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <jmlive/jmlive.h>
#include <rexglue_guide/guide.h>

#include <rex/cvar.h>
#include <rex/kernel/xam/private.h>
#include "cc2/kernel/xam/xam_avatar.h"

#include "avatars/closet.h"
#include "cc2/kernel/xnet.h"
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xio.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

// Decodes the gamer picture (user_gamerpic cvar, else the Guide's).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include "stb_image.h"

REXCVAR_DEFINE_UINT32(user_language, 1, "Kernel", "User's language ID");
REXCVAR_DEFINE_STRING(user_name, "User", "Kernel",
                      "Gamertag, max 15 chars; set in <user_data_root>/user.toml");
REXCVAR_DEFINE_STRING(user_gamerpic, "", "Kernel",
                      "Path to an image (PNG/JPG/BMP/TGA) served as the profile's gamer picture "
                      "(64x64 recommended). Relative paths resolve against the exe folder. "
                      "Empty uses the Guide's gamerpic.");

namespace cc2::kernel::xam {
namespace {
using Clock = std::chrono::steady_clock;
std::mutex g_local_mutex;
Clock::time_point g_local_checked[4];
bool g_local_connected[4] = {};  // a controller on the user index
bool g_local_signed_in[4] = {};  // connected, with a profile chosen in the Guide
bool g_local_seen[4] = {};
int g_signin_player = -1;  // the player whose Guide sign-in is up, -1 for none
bool g_signin_opened = false;
Clock::time_point g_signin_at;

// Users 1-3: a controller and a Guide profile, refreshed at most four times a second. A change is a sign-in change.
void RefreshLocalPlayer(uint32_t user_index) {
  uint32_t mask = 0;
  bool signed_in = false, changed = false, connected = false;
  {
    std::lock_guard lock(g_local_mutex);
    const auto now = Clock::now();
    if (now - g_local_checked[user_index] < std::chrono::milliseconds(250)) return;
    g_local_checked[user_index] = now;
    auto* input = static_cast<rex::input::InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
    rex::input::X_INPUT_CAPABILITIES caps{};
    connected = input && input->GetCapabilities(user_index, 0, &caps) == X_ERROR_SUCCESS;
    signed_in = connected && rexglue_guide::SignedIn(int(user_index));
    changed = g_local_seen[user_index] && signed_in != g_local_signed_in[user_index];
    g_local_seen[user_index] = true;
    g_local_connected[user_index] = connected;
    g_local_signed_in[user_index] = signed_in;
    mask = 1;
    for (uint32_t u = 1; u < 4; ++u) mask |= g_local_signed_in[u] ? 1u << u : 0u;
  }
  if (changed) {
    REXKRNL_INFO("[signin] player {} {}; signed-in mask {:#x}", user_index + 1,
                 signed_in ? "signed in as '" + jmlive::Gamertag(int(user_index)) + "'"
                           : std::string(connected ? "signed out" : "controller gone, signed out"),
                 mask);
    REX_KERNEL_STATE()->BroadcastNotification(0x0000000A, mask);  // XN_SYS_SIGNINCHANGED
    REX_KERNEL_STATE()->BroadcastNotification(0x00000009, 0);     // XN_SYS_UI off
  }
}

// A player 2-4 sign-in the Guide shows for the game ends when the Guide closes; the game is told its UI is down.
void WatchSignin() {
  bool ended = false;
  {
    std::lock_guard lock(g_local_mutex);
    if (g_signin_player < 0) return;
    const bool open = rexglue_guide::IsOpen();
    if (!g_signin_opened && open) {
      g_signin_opened = true;
    } else if ((g_signin_opened && !open) || (!g_signin_opened && Clock::now() - g_signin_at > std::chrono::seconds(5))) {
      REXKRNL_INFO("[signin] player {} sign-in closed", g_signin_player + 1);
      g_signin_player = -1;
      ended = true;
    }
  }
  if (ended) REX_KERNEL_STATE()->BroadcastNotification(0x00000009, 0);  // XN_SYS_UI off
}
}  // namespace

bool LocalPlayer(uint32_t user_index) {
  if (user_index == 0 || user_index >= 4) return false;
  RefreshLocalPlayer(user_index);
  WatchSignin();
  std::lock_guard lock(g_local_mutex);
  return g_local_signed_in[user_index];
}

// The lowest player 2-4 with a controller and no profile yet; -1 for none.
static int UnsignedLocalPlayer() {
  for (uint32_t u = 1; u < 4; ++u) {
    LocalPlayer(u);
    std::lock_guard lock(g_local_mutex);
    if (g_local_connected[u] && !g_local_signed_in[u]) return int(u);
  }
  return -1;
}

// Opens the Guide's sign-in for the lowest unsigned player 2-4; false when there is none.
bool ShowLocalPlayerSignIn() {
  const int user = UnsignedLocalPlayer();
  if (user < 0) return false;
  {
    std::lock_guard lock(g_local_mutex);
    g_signin_player = user;
    g_signin_opened = false;
    g_signin_at = Clock::now();
  }
  REXKRNL_INFO("[signin] the game asked for sign-in; opening the Guide's sign-in for player {}", user + 1);
  jmlive::ShowSignIn(user);
  return true;
}

uint64_t LocalPlayerXuid(uint32_t user_index) {
  return REX_KERNEL_STATE()->user_profile()->xuid() + user_index;
}

uint32_t SignedInUserMask() {
  uint32_t mask = 1;
  for (uint32_t u = 1; u < 4; ++u) mask |= LocalPlayer(u) ? 1u << u : 0u;
  return mask;
}

// The title screen waits on the sign-in UI without asking for sign-in state, so changes are pushed, not polled.
void PollLocalPlayers() {
  for (uint32_t u = 1; u < 4; ++u) RefreshLocalPlayer(u);
  WatchSignin();
}
}  // namespace cc2::kernel::xam

namespace rex {
namespace kernel {
namespace xam {
// The runtime's profile only knows local sign-in; the title is always on
// LIVE with an online-format XUID so the LIVE-gated flows accept it.
static constexpr uint64_t kOnlineXuid = 0x0009BABEBABEBABEULL;
static constexpr uint32_t kSigninStateLive = 2;

// Peers in a session are told apart by XUID, so each Game Jolt account gets its own online-format one.
static uint64_t OnlineXuid() {
  const std::string name = cc2::online::SignedInName();
  return name.empty() ? kOnlineXuid : cc2::lobby::XuidForName(name);
}

// Players 2-4 are on Live with player 1, as console guests are: their Game Jolt account's XUID, else player 1's with
// the player number mixed in.
static uint64_t LocalPlayerOnlineXuid(uint32_t user_index) {
  const std::string name = jmlive::GameJolt(int(user_index)).username;
  return name.empty() ? OnlineXuid() ^ user_index : cc2::lobby::XuidForName(name);
}

using namespace rex::system;
using namespace rex::system::xam;
using namespace cc2::kernel;
using namespace cc2::kernel::xam;

i32 XamUserGetXUID_entry(u32 user_index, u32 type_mask, mapped_u64 xuid_ptr) {
  assert_true(type_mask == 1 || type_mask == 2 || type_mask == 3 || type_mask == 4 ||
              type_mask == 7);
  if (!xuid_ptr) {
    return X_E_INVALIDARG;
  }
  uint32_t result = X_E_NO_SUCH_USER;
  uint64_t xuid = 0;
  if (user_index < 4) {
    if (LocalPlayer(user_index)) {
      xuid = (type_mask & (2 | 4)) && cc2::online::ReportLive() ? LocalPlayerOnlineXuid(user_index)
                                                                 : LocalPlayerXuid(user_index);
      result = X_E_SUCCESS;
    } else if (user_index == 0) {
      const auto& user_profile = REX_KERNEL_STATE()->user_profile();
      auto type = user_profile->type() & type_mask;
      if (type & (2 | 4)) {
        // Online profile: return the online-format XUID (0x0009...).
        xuid = OnlineXuid();
        result = X_E_SUCCESS;
      } else if (type & 1) {
        // maybe offline profile?
        xuid = user_profile->xuid();
        result = X_E_SUCCESS;
      }
    }
  } else {
    result = X_E_INVALIDARG;
  }
  *xuid_ptr = xuid;
  return result;
}

// The Avatar Editor (system title 584D07D1) branches on the signin state when
// saving: SignedInToLive (2) routes into a LIVE profile-sync flow whose async
// op comes from unimplemented LIVE APIs, so the save screen polls a null op and
// crashes. SignedInLocally (1), as an offline console reports, takes the local
// save path. State 2 is reported only while online::ReportLive.
u32 XamUserGetSigninState_entry(u32 user_index) {
  uint32_t signin_state = 0;
  if (user_index < 4) {
    if (user_index == 0) {
      // Signed in locally (1) until online::ReportLive.
      signin_state = cc2::online::ReportLive() ? kSigninStateLive : 1;
    } else {
      signin_state = LocalPlayer(user_index) ? (cc2::online::ReportLive() ? kSigninStateLive : 1) : 0;
    }
  }
  return signin_state;
}

typedef struct {
  rex::be<uint64_t> xuid;
  rex::be<uint32_t> info_flags;  // XUSER_INFO_FLAGS; bit 0 = XUSER_INFO_FLAG_LIVE_ENABLED
  rex::be<uint32_t> signin_state;
  rex::be<uint32_t> unk10;  // ?
  rex::be<uint32_t> unk14;  // ?
  char name[16];
} X_USER_SIGNIN_INFO;
static_assert_size(X_USER_SIGNIN_INFO, 40);

// The shown gamertag only; the profile keeps its own name so saves under profile/User stay put.
std::string Gamertag(uint32_t user_index = 0) {
  std::string name;
  // Player 1: the Game Jolt name, else the local profile name. Players 2-4: their Guide profile's name, else
  // "Player N".
  std::string source = user_index ? jmlive::Gamertag(int(user_index)) : cc2::online::SignedInName();
  if (source.empty()) source = user_index ? "Player " + std::to_string(user_index + 1) : REXCVAR_GET(user_name);
  for (char c : source) {
    if (c >= 0x20 && c <= 0x7E) name += c;
    if (name.size() == 15) break;
  }
  return name.empty() ? "User" : name;
}

i32 XamUserGetSigninInfo_entry(u32 user_index, u32 flags, ppc_ptr_t<X_USER_SIGNIN_INFO> info) {
  if (!info) {
    return X_E_INVALIDARG;
  }

  std::memset(info, 0, sizeof(X_USER_SIGNIN_INFO));
  if (user_index && LocalPlayer(user_index)) {
    const bool live = cc2::online::ReportLive();
    info->xuid = live ? LocalPlayerOnlineXuid(user_index) : LocalPlayerXuid(user_index);
    info->signin_state = live ? kSigninStateLive : 1;
    info->info_flags = live ? 0x00000001u : 0u;
    rex::string::copy_truncating(info->name, Gamertag(user_index), rex::countof(info->name));
    REXKRNL_DEBUG("[signin] XamUserGetSigninInfo({}, {:#x}) -> {:016X} '{}'", user_index, flags,
                  uint64_t(info->xuid), Gamertag(user_index));
    return X_E_SUCCESS;
  }
  if (user_index) {
    return X_E_NO_SUCH_USER;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  const uint32_t signin_state = cc2::online::ReportLive() ? kSigninStateLive : 1;
  // Signed in to LIVE: report the online-format XUID (0x0009...); LIVE-gated
  // titles reject the offline XUID.
  info->xuid = (signin_state == 2) ? OnlineXuid() : user_profile->xuid();
  info->signin_state = signin_state;
  // dwInfoFlags bit 0 (LIVE_ENABLED) must be set while signed in to LIVE.
  // Online-gated titles reject the profile without it.
  info->info_flags = (signin_state == 2) ? 0x00000001u : 0u;
  rex::string::copy_truncating(info->name, Gamertag(), rex::countof(info->name));
  return X_E_SUCCESS;
}

u32 XamUserGetName_entry(u32 user_index, mapped_string buffer, u32 buffer_len) {
  if (user_index >= 4) {
    return X_E_INVALIDARG;
  }

  if (user_index && !LocalPlayer(user_index)) {
    return X_E_NO_SUCH_USER;
  }

  rex::string::copy_truncating(buffer, Gamertag(user_index), std::min(buffer_len, uint32_t(16)));
  return X_E_SUCCESS;
}

u32 XamUserGetGamerTag_entry(u32 user_index, mapped_wstring buffer, u32 buffer_len) {
  if (user_index >= 4) {
    return X_E_INVALIDARG;
  }

  if (user_index && !LocalPlayer(user_index)) {
    return X_E_NO_SUCH_USER;
  }

  if (!buffer || buffer_len < 16) {
    return X_E_INVALIDARG;
  }

  auto user_name = rex::string::to_utf16(Gamertag(user_index));
  rex::string::copy_and_swap_truncating(buffer, user_name, std::min(buffer_len, uint32_t(16)));
  return X_E_SUCCESS;
}

constexpr uint32_t kAvatarInfo1SettingId = 0x63E80044u;  // XPROFILE_GAMERCARD_AVATAR_INFO_1

typedef struct {
  rex::be<uint32_t> setting_count;
  rex::be<uint32_t> settings_ptr;
} X_USER_READ_PROFILE_SETTINGS;
static_assert_size(X_USER_READ_PROFILE_SETTINGS, 8);

// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Tools/Generic/xboxtools.cpp
uint32_t XamUserReadProfileSettingsEx(uint32_t title_id, uint32_t user_index, uint32_t xuid_count,
                                      be<uint64_t>* xuids, uint32_t setting_count,
                                      be<uint32_t>* setting_ids, uint32_t unk,
                                      be<uint32_t>* buffer_size_ptr, uint8_t* buffer,
                                      XAM_OVERLAPPED* overlapped) {
  if (!xuid_count) {
    assert_null(xuids);
  } else {
    assert_true(xuid_count == 1);
    assert_not_null(xuids);
    // Other XUIDs are lobby peers; they get the local settings except the avatar (see below).
    xuid_count = 1;
  }
  assert_zero(unk);  // probably flags

  // must have at least 1 to 32 settings
  if (setting_count < 1 || setting_count > 32) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // buffer size pointer must be valid
  if (!buffer_size_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // if buffer size is non-zero, buffer pointer must be valid
  auto buffer_size = static_cast<uint32_t>(*buffer_size_ptr);
  if (buffer_size && !buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint32_t needed_header_size = 0;
  uint32_t needed_data_size = 0;
  for (uint32_t i = 0; i < setting_count; ++i) {
    needed_header_size += sizeof(X_USER_PROFILE_SETTING);
    UserProfile::Setting::Key setting_key;
    setting_key.value = static_cast<uint32_t>(setting_ids[i]);
    switch (static_cast<UserProfile::Setting::Type>(setting_key.type)) {
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::BINARY:
        needed_data_size += setting_key.size;
        break;
      default:
        break;
    }
  }
  if (xuids) {
    needed_header_size *= xuid_count;
    needed_data_size *= xuid_count;
  }
  needed_header_size += sizeof(X_USER_READ_PROFILE_SETTINGS);

  uint32_t needed_size = needed_header_size + needed_data_size;
  if (!buffer || buffer_size < needed_size) {
    if (!buffer_size) {
      *buffer_size_ptr = needed_size;
    }
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  // Title ID = 0 means us.
  // 0xfffe07d1 = profile?

  if (!xuids && user_index && !LocalPlayer(user_index)) {
    // Only support user 0 and signed-in local players.
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(
          REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();

  // Back XPROFILE_GAMERCARD_AVATAR_INFO_1 with the persisted Avatar Editor
  // manifest before validity checking -- titles (e.g. Crash Course 2) read
  // the player's avatar from this profile setting at boot.
  EnsureAvatarProfileSetting(user_profile);

  // First call asks for size (fill buffer_size_ptr).
  // Second call asks for buffer contents with that size.

  // TODO(gibbed): setting validity checking without needing a user profile
  // object.
  bool any_missing = false;
  for (uint32_t i = 0; i < setting_count; ++i) {
    auto setting_id = static_cast<uint32_t>(setting_ids[i]);
    auto setting = user_profile->GetSetting(setting_id);
    if (!setting) {
      any_missing = true;
      REXKRNL_ERROR(
          "xeXamUserReadProfileSettingsEx requested unimplemented setting "
          "{:08X}",
          setting_id);
    }
  }
  if (any_missing) {
    // TODO(benvanik): don't fail? most games don't even check!
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(
          REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_INVALID_PARAMETER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_INVALID_PARAMETER;
  }

  // A remote XUID is a lobby peer. The game reads its avatar here (TU1 SafeAvatarMetadataTask, 0x82AE1CF8), and
  // falls back to a random avatar when the reply is not a 1000-byte binary, so an unknown peer stays unset.
  const uint64_t request_xuid = xuids ? static_cast<uint64_t>(xuids[0]) : 0;
  // Players 2-4 are read by user index or by either of their XUIDs; their avatar is their own.
  uint32_t local_user = !xuids && user_index < 4 ? uint32_t(user_index) : 0u;
  for (uint32_t u = 1; xuids && u < 4; ++u) {
    if (LocalPlayer(u) && (request_xuid == LocalPlayerXuid(u) || request_xuid == LocalPlayerOnlineXuid(u))) {
      local_user = u;
    }
  }
  const bool remote = xuids && !local_user && request_xuid != user_profile->xuid() && request_xuid != OnlineXuid();
  bool wants_avatar = false;
  for (uint32_t i = 0; i < setting_count; ++i) {
    wants_avatar |= static_cast<uint32_t>(setting_ids[i]) == kAvatarInfo1SettingId;
  }

  // The peer's manifest comes from the lobby cache, else one bounded heartbeat right here. The read always completes
  // before returning with every setting written; an unknown avatar goes back unset and the game picks a random one.
  uint8_t peer_avatar[cc2::lobby::kAvatarManifestSize];
  bool have_peer = false;
  if (local_user && wants_avatar) {
    have_peer = LocalPlayerAvatarManifest(local_user, peer_avatar);
  } else if (remote && wants_avatar) {
    have_peer = cc2::lobby::FetchPeerAvatar(request_xuid, peer_avatar, 2000);
    REXKRNL_INFO("[avatar] profile avatar for lobby peer {:016X}: {}", request_xuid,
                 have_peer ? "from the lobby" : "unknown, left unset");
  }

  auto out_header = reinterpret_cast<X_USER_READ_PROFILE_SETTINGS*>(buffer);
  auto out_setting = reinterpret_cast<X_USER_PROFILE_SETTING*>(&out_header[1]);
  out_header->setting_count = static_cast<uint32_t>(setting_count);
  out_header->settings_ptr = REX_KERNEL_MEMORY()->HostToGuestVirtual(out_setting);

  UserProfile::SettingByteStream out_stream(REX_KERNEL_MEMORY()->HostToGuestVirtual(buffer), buffer,
                                            buffer_size, needed_header_size);
  for (uint32_t i = 0; i < setting_count; ++i) {
    const uint32_t setting_id = setting_ids[i];
    auto setting = user_profile->GetSetting(setting_id);
    std::memset(out_setting, 0, sizeof(X_USER_PROFILE_SETTING));
    if (xuids) {
      out_setting->xuid = remote || local_user ? request_xuid : user_profile->xuid();
    } else {
      out_setting->user_index = static_cast<uint32_t>(user_index);
    }
    out_setting->setting_id = setting_id;
    if ((remote || local_user) && setting_id == kAvatarInfo1SettingId) {
      if (have_peer) {
        UserProfile::BinarySetting peer(
            setting_id,
            std::vector<uint8_t>(peer_avatar, peer_avatar + cc2::lobby::kAvatarManifestSize));
        out_setting->from = 1;
        peer.Append(&out_setting->data, &out_stream);
      }
    } else {
      out_setting->from = !setting || !setting->is_set ? 0 : setting->is_title_specific() ? 2 : 1;
      if (setting && setting->is_set) {
        setting->Append(&out_setting->data, &out_stream);
      }
    }
    ++out_setting;
  }

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(
        REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserReadProfileSettings_entry(u32 title_id, u32 user_index, u32 xuid_count, mapped_u64 xuids,
                                     u32 setting_count, mapped_u32 setting_ids,
                                     mapped_u32 buffer_size_ptr, mapped_void buffer_ptr,
                                     ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, 0, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserReadProfileSettingsEx_entry(u32 title_id, u32 user_index, u32 xuid_count,
                                       mapped_u64 xuids, u32 setting_count, mapped_u32 setting_ids,
                                       mapped_u32 buffer_size_ptr, u32 unk_2,
                                       mapped_void buffer_ptr,
                                       ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, unk_2, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserWriteProfileSettings_entry(u32 title_id, u32 user_index, u32 setting_count,
                                      ppc_ptr_t<X_USER_PROFILE_SETTING> settings,
                                      ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  if (!setting_count || !settings) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index && LocalPlayer(user_index)) {
    // Players 2-4 share the profile; their writes are accepted and dropped.
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(), X_ERROR_SUCCESS);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_SUCCESS;
  }
  if (user_index) {
    // Only support user 0.
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(),
                                                      X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  // Update and save settings.
  const auto& user_profile = REX_KERNEL_STATE()->user_profile();

  for (uint32_t n = 0; n < setting_count; ++n) {
    const X_USER_PROFILE_SETTING& setting = settings[n];

    auto setting_type = static_cast<UserProfile::Setting::Type>(setting.data.type);
    if (setting_type == UserProfile::Setting::Type::UNSET) {
      continue;
    }

    REXKRNL_DEBUG(
        "XamUserWriteProfileSettings: setting index [{}]:"
        " from={} setting_id={:08X} data.type={}",
        n, (uint32_t)setting.from, (uint32_t)setting.setting_id, setting.data.type);

    switch (setting_type) {
      case UserProfile::Setting::Type::CONTENT:
      case UserProfile::Setting::Type::BINARY: {
        uint8_t* binary_ptr = REX_KERNEL_MEMORY()->TranslateVirtual(setting.data.binary.ptr);
        size_t binary_size = setting.data.binary.size;
        std::vector<uint8_t> bytes;
        if (setting.data.binary.ptr) {
          // Copy provided data
          bytes.resize(binary_size);
          std::memcpy(bytes.data(), binary_ptr, binary_size);
        } else {
          // Data pointer was NULL, so fill with zeroes
          bytes.resize(binary_size, 0);
        }
        user_profile->AddSetting(
            std::make_unique<xam::UserProfile::BinarySetting>(setting.setting_id, bytes));
      } break;
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::DOUBLE:
      case UserProfile::Setting::Type::FLOAT:
      case UserProfile::Setting::Type::INT32:
      case UserProfile::Setting::Type::INT64:
      case UserProfile::Setting::Type::DATETIME:
      default: {
        REXKRNL_ERROR("XamUserWriteProfileSettings: Unimplemented data type {}", setting_type);
      } break;
    };
  }

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(), X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserCheckPrivilege_entry(u32 user_index, u32 mask, mapped_u32 out_value) {
  // checking all users?
  if (user_index != 0xFF) {
    if (user_index >= 4) {
      return X_ERROR_INVALID_PARAMETER;
    }

    if (user_index && !LocalPlayer(user_index)) {
      return X_ERROR_NO_SUCH_USER;
    }
  }

  // Grant all privileges. LIVE-required titles (e.g. DCC2) re-prompt sign-in
  // indefinitely if online privileges are denied.
  *out_value = 1;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetFlags_entry(u32 user_index, mapped_u32 out_flags) {
  if (user_index && !LocalPlayer(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }

  // No restrictions?
  *out_flags = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetRating_entry(u32 user_index, u32 unk1, mapped_u32 out_unk2,
                                             mapped_u32 out_unk3) {
  if (user_index && !LocalPlayer(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }

  // Some games have special case paths for 3F that differ from the failure
  // path, so my guess is that's 'don't care'.
  *out_unk2 = 0x3F;
  *out_unk3 = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionCheckAccess_entry(u32 user_index, u32 unk1, u32 unk2, u32 unk3,
                                               u32 unk4, mapped_u32 out_unk5, u32 overlapped_ptr) {
  *out_unk5 = 1;

  if (overlapped_ptr) {
    // TODO(benvanik): does this need the access arg on it?
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
  }

  return X_ERROR_SUCCESS;
}

u32 XamUserIsOnlineEnabled_entry(u32 user_index) {
  return 1;
}

u32 XamUserGetMembershipTier_entry(u32 user_index) {
  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (user_index && !LocalPlayer(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }
  return 6 /* 6 appears to be Gold */;
}

u32 XamUserAreUsersFriends_entry(u32 user_index, u32 unk1, u32 unk2, mapped_u32 out_value,
                                 u32 overlapped_ptr) {
  uint32_t are_friends = 0;
  X_RESULT result;

  if (user_index >= 4) {
    result = X_ERROR_INVALID_PARAMETER;
  } else {
    if (user_index == 0) {
      const auto& user_profile = REX_KERNEL_STATE()->user_profile();
      if (user_profile->signin_state() == 0) {
        result = X_ERROR_NOT_LOGGED_ON;
      } else {
        // No friends!
        are_friends = 0;
        result = X_ERROR_SUCCESS;
      }
    } else {
      // Only support user 0.
      result = X_ERROR_NO_SUCH_USER;  // if user is local -> X_ERROR_NOT_LOGGED_ON
    }
  }

  if (out_value) {
    assert_true(!overlapped_ptr);
    *out_value = result == X_ERROR_SUCCESS ? are_friends : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!out_value);
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(
        overlapped_ptr, result == X_ERROR_SUCCESS ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED,
        X_HRESULT_FROM_WIN32(result), result == X_ERROR_SUCCESS ? are_friends : 0);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamShowSigninUI_entry(u32 unk, u32 unk_mask) {
  // Mask values vary, probably local/remote user types.
  // The Guide's Game Jolt sign-in stands in for the console's.
  if (!cc2::online::ReportLive()) {
    cc2::online::RequestSignIn(unk, unk_mask);
    return X_ERROR_SUCCESS;
  }
  // Player 1 on Live: a controller without a profile picks one in its own Guide.
  if (ShowLocalPlayerSignIn()) return X_ERROR_SUCCESS;

  // To fix game modes that display a 4 profile signin UI (even if playing
  // alone):
  // XN_SYS_SIGNINCHANGED
  REX_KERNEL_STATE()->BroadcastNotification(0x0000000A, SignedInUserMask());
  // Games seem to sit and loop until we trigger this notification:
  // XN_SYS_UI (off)
  REX_KERNEL_STATE()->BroadcastNotification(0x00000009, 0);
  // XN_LIVE_CONNECTIONCHANGED = connected. LIVE-required titles (e.g. DCC2)
  // sit in an online-init loop until they're told the LIVE link is up.
  REX_KERNEL_STATE()->BroadcastNotification(0x00000014, 1);
  return X_ERROR_SUCCESS;
}

// TODO(gibbed): probably a FILETIME/LARGE_INTEGER, unknown currently
struct X_ACHIEVEMENT_UNLOCK_TIME {
  rex::be<uint32_t> unk_0;
  rex::be<uint32_t> unk_4;
};

struct X_ACHIEVEMENT_DETAILS {
  rex::be<uint32_t> id;
  rex::be<uint32_t> label_ptr;
  rex::be<uint32_t> description_ptr;
  rex::be<uint32_t> unachieved_ptr;
  rex::be<uint32_t> image_id;
  rex::be<uint32_t> gamerscore;
  X_ACHIEVEMENT_UNLOCK_TIME unlock_time;
  rex::be<uint32_t> flags;

  static const size_t kStringBufferSize = 464;
};
static_assert_size(X_ACHIEVEMENT_DETAILS, 36);

class XStaticAchievementEnumerator : public XEnumerator {
 public:
  struct AchievementDetails {
    uint32_t id;
    std::u16string label;
    std::u16string description;
    std::u16string unachieved;
    uint32_t image_id;
    uint32_t gamerscore;
    struct {
      uint32_t unk_0;
      uint32_t unk_4;
    } unlock_time;
    uint32_t flags;
  };

  XStaticAchievementEnumerator(KernelState* kernel_state, size_t items_per_enumerate,
                               uint32_t flags)
      : XEnumerator(kernel_state, items_per_enumerate,
                    sizeof(X_ACHIEVEMENT_DETAILS) +
                        (!!(flags & 7) ? X_ACHIEVEMENT_DETAILS::kStringBufferSize : 0)),
        flags_(flags) {}

  void AppendItem(AchievementDetails item) { items_.push_back(std::move(item)); }

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data, uint32_t* written_count) override {
    size_t count = std::min(items_.size() - current_item_, items_per_enumerate());
    if (!count) {
      return X_ERROR_NO_MORE_FILES;
    }

    size_t size = count * item_size();

    auto details = reinterpret_cast<X_ACHIEVEMENT_DETAILS*>(buffer_data);
    size_t string_offset = items_per_enumerate() * sizeof(X_ACHIEVEMENT_DETAILS);
    auto string_buffer =
        StringBuffer{buffer_ptr + static_cast<uint32_t>(string_offset), &buffer_data[string_offset],
                     count * X_ACHIEVEMENT_DETAILS::kStringBufferSize};
    for (size_t i = 0, o = current_item_; i < count; ++i, ++current_item_) {
      const auto& item = items_[current_item_];
      details[i].id = item.id;
      details[i].label_ptr = !!(flags_ & 1) ? AppendString(string_buffer, item.label) : 0;
      details[i].description_ptr =
          !!(flags_ & 2) ? AppendString(string_buffer, item.description) : 0;
      details[i].unachieved_ptr = !!(flags_ & 4) ? AppendString(string_buffer, item.unachieved) : 0;
      details[i].image_id = item.image_id;
      details[i].gamerscore = item.gamerscore;
      details[i].unlock_time.unk_0 = item.unlock_time.unk_0;
      details[i].unlock_time.unk_4 = item.unlock_time.unk_4;
      details[i].flags = item.flags;
    }

    if (written_count) {
      *written_count = static_cast<uint32_t>(count);
    }

    return X_ERROR_SUCCESS;
  }

 private:
  struct StringBuffer {
    uint32_t ptr;
    uint8_t* data;
    size_t remaining_bytes;
  };

  uint32_t AppendString(StringBuffer& sb, const std::u16string_view string) {
    size_t count = string.length() + 1;
    size_t size = count * sizeof(char16_t);
    if (size > sb.remaining_bytes) {
      assert_always();
      return 0;
    }
    auto ptr = sb.ptr;
    rex::string::copy_and_swap_truncating(reinterpret_cast<char16_t*>(sb.data), string, count);
    sb.ptr += static_cast<uint32_t>(size);
    sb.data += size;
    sb.remaining_bytes -= size;
    return ptr;
  }

 private:
  uint32_t flags_;
  std::vector<AchievementDetails> items_;
  size_t current_item_ = 0;
};

u32 XamUserCreateAchievementEnumerator_entry(u32 title_id, u32 user_index, u32 xuid, u32 flags,
                                             u32 offset, u32 count, mapped_u32 buffer_size_ptr,
                                             mapped_u32 handle_ptr) {
  if (!count || !buffer_size_ptr || !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t entry_size = sizeof(X_ACHIEVEMENT_DETAILS);
  if (flags & 7) {
    entry_size += X_ACHIEVEMENT_DETAILS::kStringBufferSize;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = static_cast<uint32_t>(entry_size) * count;
  }

  auto e = object_ref<XStaticAchievementEnumerator>(
      new XStaticAchievementEnumerator(REX_KERNEL_STATE(), count, flags));
  auto result = e->Initialize(user_index, 0xFB, 0xB000A, 0xB000B, 0);
  if (XFAILED(result)) {
    return result;
  }

  const util::XdbfGameData db = REX_KERNEL_STATE()->title_xdbf();

  if (db.is_valid()) {
    const XLanguage language =
        db.GetExistingLanguage(static_cast<XLanguage>(REXCVAR_GET(user_language)));
    const std::vector<util::XdbfAchievementTableEntry> achievement_list = db.GetAchievements();

    for (const util::XdbfAchievementTableEntry& entry : achievement_list) {
      auto item = XStaticAchievementEnumerator::AchievementDetails{
          entry.id,
          rex::string::to_utf16(db.GetStringTableEntry(language, entry.label_id)),
          rex::string::to_utf16(db.GetStringTableEntry(language, entry.description_id)),
          rex::string::to_utf16(db.GetStringTableEntry(language, entry.unachieved_id)),
          entry.image_id,
          entry.gamerscore,
          {0, 0},
          entry.flags};

      e->AppendItem(item);
    }
  }

  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}

// The Avatar Editor's wardrobe sanitizer (sub_920BFF80) swaps any worn award item
// ((id.c >> 8) & 0xF == 1) for a stock default unless this enumerator reports it granted.
// Entries carry the verbatim 16-byte asset id at +4 and flags at +44 (0x20000 = granted).
struct X_AVATAR_AWARD_ENTRY {
  rex::be<uint32_t> title_id;    // +0
  avatars::AssetId id;           // +4..+19, verbatim (matched as 2 qwords)
  uint8_t reserved[24];          // +20
  rex::be<uint32_t> flags;       // +44
  rex::be<uint32_t> reserved2;   // +48
};
static_assert(sizeof(X_AVATAR_AWARD_ENTRY) == 52, "award entry is 52 bytes");

u32 XamUserCreateAvatarAssetEnumerator_entry(u32 user_index, u32 title_id, u32 unk3, u32 unk4,
                                             u32 unk5, u32 item_count, mapped_u32 buffer_size_ptr,
                                             mapped_u32 handle_ptr) {
  if (!handle_ptr || !item_count || user_index >= 4) {
    if (buffer_size_ptr) {
      *buffer_size_ptr = 0;
    }
    return X_ERROR_INVALID_PARAMETER;
  }
  if (buffer_size_ptr) {
    *buffer_size_ptr = static_cast<uint32_t>(sizeof(X_AVATAR_AWARD_ENTRY)) * item_count;
  }
  auto e = make_object<XStaticEnumerator<X_AVATAR_AWARD_ENTRY>>(REX_KERNEL_STATE(), item_count);
  auto result = e->Initialize(user_index, 0xFB, 0x20005, 0x20007, 0);
  if (XFAILED(result)) {
    return result;
  }
  uint32_t served = 0;
  for (const auto& item : avatars::GetCloset().items()) {
    const uint32_t c = item.id.c.get();
    if (((c >> 8) & 0xFu) != 1u) {
      continue;  // marketplace purchase, not an award
    }
    const uint32_t item_title = (uint32_t(item.id.d[4]) << 24) | (uint32_t(item.id.d[5]) << 16) |
                                (uint32_t(item.id.d[6]) << 8) | uint32_t(item.id.d[7]);
    if (title_id && item_title != title_id) {
      continue;
    }
    auto* entry = e->AppendItem();
    *entry = {};
    entry->title_id = item_title;
    entry->id = item.id;
    entry->flags = 0x20000u;  // granted
    ++served;
  }
  REXKRNL_INFO("[avatar] XamUserCreateAvatarAssetEnumerator(user={}, title={:08X}) -> {} award(s)",
               user_index, title_id, served);
  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}

u32 XamUserCreateStatsEnumerator_entry(u32 title_id, u32 enumerator_type, u64 pivot_user,
                                       u32 num_rows, u32 num_stats_specs,
                                       ppc_ptr_t<X_USER_STATS_SPEC> specs,
                                       mapped_u32 buffer_size_ptr, mapped_u32 handle_ptr) {
  // Must hand back a valid (empty) leaderboard: titles enumerate the handle and
  // parse the X_USER_STATS_READ_RESULTS without validating it (DCC2
  // NETMANLA::UpdateFriendsLeaderboardsList qsorts view->rows with whatever
  // num_rows/rows_ptr it finds in the buffer).
  if (!handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  *handle_ptr = 0;

  if (!buffer_size_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  *buffer_size_ptr = 0;

  if (!pivot_user || !specs) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!num_rows || num_rows > X_STATS_MAX_ROW_COUNT) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!num_stats_specs || enumerator_type > X_STATS_ENUMERATOR_TYPE::BY_RATING) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto* memory = REX_KERNEL_MEMORY();

  const uint32_t views_size =
      num_stats_specs * static_cast<uint32_t>(sizeof(X_USER_STATS_VIEW));
  const uint32_t views_address = memory->SystemHeapAlloc(views_size);
  auto* views = memory->TranslateVirtual<X_USER_STATS_VIEW*>(views_address);
  std::memset(views, 0, views_size);

  const X_USER_STATS_SPEC* spec_array = specs;
  for (uint32_t i = 0; i < num_stats_specs; ++i) {
    X_USER_STATS_VIEW& view = views[i];
    view.view_id = spec_array[i].view_id;
    view.total_view_rows = 0;
    view.num_rows = 0;
    // Some titles dereference rows_ptr even with zero rows -- keep it valid.
    const uint32_t rows_address =
        memory->SystemHeapAlloc(static_cast<uint32_t>(sizeof(X_USER_STATS_ROW)));
    auto* row = memory->TranslateVirtual<X_USER_STATS_ROW*>(rows_address);
    std::memset(row, 0, sizeof(X_USER_STATS_ROW));
    view.rows_ptr = rows_address;
  }

  auto e = make_object<XStaticEnumerator<X_USER_STATS_READ_RESULTS>>(REX_KERNEL_STATE(), 1);
  auto result = e->Initialize(0xFF, 0xFB, 0xB0023, 0xB0024, 0);
  if (XFAILED(result)) {
    return result;
  }

  auto* results = e->AppendItem();
  results->num_views = num_stats_specs;
  results->views_ptr = views_address;

  *buffer_size_ptr = static_cast<uint32_t>(sizeof(X_USER_STATS_READ_RESULTS)) + views_size;
  *handle_ptr = e->handle();

  REXKRNL_DEBUG("XamUserCreateStatsEnumerator({:08X}, type={}, pivot={:016X}, rows={}, specs={}) "
                "-> empty leaderboard",
                title_id, enumerator_type, pivot_user, num_rows, num_stats_specs);
  return X_ERROR_SUCCESS;
}

u32 XamParseGamerTileKey_entry(mapped_u32 key_ptr, mapped_u32 out1_ptr, mapped_u32 out2_ptr,
                               mapped_u32 out3_ptr) {
  REXKRNL_DEBUG("XamParseGamerTileKey()");
  *out1_ptr = 0xC0DE0001;
  *out2_ptr = 0xC0DE0002;
  *out3_ptr = 0xC0DE0003;
  return X_ERROR_SUCCESS;
}

namespace {

// A gamer picture in the layout profile tiles use: linear 32-bit big-endian
// A8R8G8B8, i.e. A,R,G,B byte order.
struct CustomGamerPic {
  std::vector<uint8_t> argb;
  uint32_t width = 0;
  uint32_t height = 0;
  bool valid() const { return !argb.empty(); }
};

CustomGamerPic DecodeGamerPic(const uint8_t* data, size_t size, const std::string& what) {
  CustomGamerPic result;
  int width = 0, height = 0, channels = 0;
  stbi_uc* rgba = stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, STBI_rgb_alpha);
  if (!rgba) {
    REXKRNL_WARN("[gamerpic] {} decode failed ({})", what, stbi_failure_reason());
    return result;
  }
  result.width = static_cast<uint32_t>(width);
  result.height = static_cast<uint32_t>(height);
  const size_t pixel_count = size_t(width) * size_t(height);
  result.argb.resize(pixel_count * 4);
  for (size_t i = 0; i < pixel_count; ++i) {
    const stbi_uc* src = &rgba[i * 4];
    uint8_t* dst = &result.argb[i * 4];
    dst[0] = src[3];  // A
    dst[1] = src[0];  // R
    dst[2] = src[1];  // G
    dst[3] = src[2];  // B
  }
  stbi_image_free(rgba);
  REXKRNL_DEBUG("[gamerpic] loaded {} ({}x{})", what, width, height);
  return result;
}

// The user_gamerpic file, decoded once on first use.
const CustomGamerPic& GetConfiguredGamerPic() {
  static const CustomGamerPic pic = [] {
    CustomGamerPic result;
    const std::string configured = REXCVAR_GET(user_gamerpic);
    if (configured.empty()) {
      return result;
    }
    std::filesystem::path path = configured;
    if (path.is_relative()) {
      path = rex::filesystem::GetExecutableFolder() / path;
    }
    std::vector<uint8_t> file_data;
    if (FILE* file = rex::filesystem::OpenFile(path, "rb")) {
      fseek(file, 0, SEEK_END);
      file_data.resize(static_cast<size_t>(ftell(file)));
      fseek(file, 0, SEEK_SET);
      fread(file_data.data(), 1, file_data.size(), file);
      fclose(file);
    }
    if (file_data.empty()) {
      REXKRNL_WARN("[gamerpic] user_gamerpic not readable: {}", path.string());
      return result;
    }
    return DecodeGamerPic(file_data.data(), file_data.size(), path.string());
  }();
  return pic;
}

// The picture tiles are served from: player 1's user_gamerpic when set, else the player's Guide gamerpic, decoded
// again when it changes.
std::shared_ptr<const CustomGamerPic> GetCustomGamerPic(uint32_t user_index) {
  static const auto configured = std::make_shared<const CustomGamerPic>(GetConfiguredGamerPic());
  if (user_index == 0 && configured->valid()) return configured;
  if (user_index >= 4) user_index = 0;
  static std::mutex mutex;
  static std::shared_ptr<const std::string> source[4];
  static std::shared_ptr<const CustomGamerPic> decoded[4];
  const std::shared_ptr<const std::string> image = jmlive::Gamerpic(int(user_index));
  std::lock_guard lock(mutex);
  if (!decoded[user_index] || image != source[user_index]) {
    source[user_index] = image;
    decoded[user_index] = std::make_shared<const CustomGamerPic>(
        image ? DecodeGamerPic(reinterpret_cast<const uint8_t*>(image->data()), image->size(),
                               fmt::format("player {}'s Guide gamerpic", user_index + 1))
              : CustomGamerPic());
  }
  return decoded[user_index];
}

}  // namespace

u32 XamReadTileToTexture_entry(u32 unknown, u32 title_id, u64 tile_id, u32 user_index,
                               mapped_void buffer_ptr, u32 stride, u32 height, u32 overlapped_ptr) {
  REXKRNL_DEBUG("XamReadTileToTexture(type={}, title={:08X}, tile_id={:016X}, user={}, stride={}, "
                "height={}, overlapped={:08X})",
                unknown, title_id, tile_id, user_index, stride, height, overlapped_ptr);
  // TODO(gibbed): unknown=0,2,3,9
  const size_t size = size_t(stride) * size_t(height);

  const auto shared_pic = GetCustomGamerPic(user_index && LocalPlayer(user_index) ? user_index : 0);
  const CustomGamerPic& pic = *shared_pic;
  if (pic.valid() && buffer_ptr && stride >= 4 && height) {
    // Serve the configured picture as the tile -- including for tile_id==0,
    // which titles pass for the local user when offline (DCC2's
    // XUserReadGamerPicture draws its built-in image when this call fails).
    // Games size the buffer as stride x height with width == stride / 4
    // (64x64 big tile, 32x32 small); each tile pixel averages the picture's
    // pixels under it.
    auto* dst = buffer_ptr.as<uint8_t*>();
    std::memset(dst, 0, size);
    const uint32_t dst_w = std::min<uint32_t>(stride / 4, 256);
    const uint32_t dst_h = std::min<uint32_t>(height, 256);
    for (uint32_t y = 0; y < dst_h; ++y) {
      uint8_t* dst_row = dst + size_t(y) * stride;
      const size_t y0 = size_t(y) * pic.height / dst_h;
      const size_t y1 = std::max(y0 + 1, size_t(y + 1) * pic.height / dst_h);
      for (uint32_t x = 0; x < dst_w; ++x) {
        const size_t x0 = size_t(x) * pic.width / dst_w;
        const size_t x1 = std::max(x0 + 1, size_t(x + 1) * pic.width / dst_w);
        uint32_t sum[4] = {};
        for (size_t sy = y0; sy < y1; ++sy) {
          for (size_t sx = x0; sx < x1; ++sx) {
            for (int c = 0; c < 4; ++c) sum[c] += pic.argb[(sy * pic.width + sx) * 4 + c];
          }
        }
        const uint32_t count = uint32_t((y1 - y0) * (x1 - x0));
        for (int c = 0; c < 4; ++c) dst_row[size_t(x) * 4 + c] = uint8_t(sum[c] / count);
      }
    }
  } else {
    if (!tile_id) {
      return X_ERROR_INVALID_PARAMETER;
    }
    std::memset(buffer_ptr, 0xFF, size);
  }

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamWriteGamerTile_entry(u32 arg1, u32 arg2, u32 arg3, u32 arg4, u32 arg5, u32 overlapped_ptr) {
  REXKRNL_DEBUG("XamWriteGamerTile({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, overlapped={:08X})",
                arg1, arg2, arg3, arg4, arg5, overlapped_ptr);
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamSessionCreateHandle_entry(mapped_u32 handle_ptr) {
  *handle_ptr = 0xCAFEDEAD;
  return X_ERROR_SUCCESS;
}

u32 XamSessionRefObjByHandle_entry(u32 handle, mapped_u32 obj_ptr) {
  assert_true(handle == 0xCAFEDEAD);
  // TODO(PermaNull): Implement this properly,
  // For the time being returning 0xDEADF00D will prevent crashing.
  *obj_ptr = 0xDEADF00D;
  return X_ERROR_SUCCESS;
}

// XamUserProfileSync(user_mask): flush/sync profile data. There is no cloud to
// sync with; local persistence already happened by the time titles call this
// (e.g. the Avatar Editor saves the manifest through XamAvatarSetManifest
// first) -- report success so save flows complete.
u32 XamUserProfileSync_entry(u32 user_mask) {
  REXKRNL_DEBUG("XamUserProfileSync(mask={:#x}) -> success (offline no-op)", user_mask);
  return 0;
}

// XamUserValidateAvatarManifest(user, X_AVATAR_METADATA*, overlapped): on the
// console this validates the manifest against LIVE (the
// validateavatarmanifest service). Offline, locally-authored manifests are
// valid by definition -- complete with success so the Avatar Editor's save
// operation advances past its validate stage.
u32 XamUserValidateAvatarManifest_entry(u32 user_index, mapped_void manifest_ptr,
                                        mapped_void overlapped_ptr) {
  REXKRNL_DEBUG("XamUserValidateAvatarManifest(user={}) -> success (offline)", user_index);
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(),
                                                    X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamUserGetXUID, rex::kernel::xam::XamUserGetXUID_entry)
REX_EXPORT(__imp__XamUserGetSigninState, rex::kernel::xam::XamUserGetSigninState_entry)
REX_EXPORT(__imp__XamUserGetSigninInfo, rex::kernel::xam::XamUserGetSigninInfo_entry)
REX_EXPORT(__imp__XamUserGetName, rex::kernel::xam::XamUserGetName_entry)
REX_EXPORT(__imp__XamUserGetGamerTag, rex::kernel::xam::XamUserGetGamerTag_entry)
REX_EXPORT(__imp__XamUserReadProfileSettings, rex::kernel::xam::XamUserReadProfileSettings_entry)
REX_EXPORT(__imp__XamUserReadProfileSettingsEx,
           rex::kernel::xam::XamUserReadProfileSettingsEx_entry)
REX_EXPORT(__imp__XamUserWriteProfileSettings, rex::kernel::xam::XamUserWriteProfileSettings_entry)
REX_EXPORT(__imp__XamUserCheckPrivilege, rex::kernel::xam::XamUserCheckPrivilege_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetFlags,
           rex::kernel::xam::XamUserContentRestrictionGetFlags_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetRating,
           rex::kernel::xam::XamUserContentRestrictionGetRating_entry)
REX_EXPORT(__imp__XamUserContentRestrictionCheckAccess,
           rex::kernel::xam::XamUserContentRestrictionCheckAccess_entry)
REX_EXPORT(__imp__XamUserIsOnlineEnabled, rex::kernel::xam::XamUserIsOnlineEnabled_entry)
REX_EXPORT(__imp__XamUserGetMembershipTier, rex::kernel::xam::XamUserGetMembershipTier_entry)
REX_EXPORT(__imp__XamUserAreUsersFriends, rex::kernel::xam::XamUserAreUsersFriends_entry)
REX_EXPORT(__imp__XamShowSigninUI, rex::kernel::xam::XamShowSigninUI_entry)
REX_EXPORT(__imp__XamUserCreateAchievementEnumerator,
           rex::kernel::xam::XamUserCreateAchievementEnumerator_entry)
REX_EXPORT(__imp__XamUserCreateStatsEnumerator,
           rex::kernel::xam::XamUserCreateStatsEnumerator_entry)
REX_EXPORT(__imp__XamUserCreateAvatarAssetEnumerator,
           rex::kernel::xam::XamUserCreateAvatarAssetEnumerator_entry)
REX_EXPORT(__imp__XamParseGamerTileKey, rex::kernel::xam::XamParseGamerTileKey_entry)
REX_EXPORT(__imp__XamReadTileToTexture, rex::kernel::xam::XamReadTileToTexture_entry)
REX_EXPORT(__imp__XamWriteGamerTile, rex::kernel::xam::XamWriteGamerTile_entry)
REX_EXPORT(__imp__XamSessionCreateHandle, rex::kernel::xam::XamSessionCreateHandle_entry)
REX_EXPORT(__imp__XamSessionRefObjByHandle, rex::kernel::xam::XamSessionRefObjByHandle_entry)

REX_EXPORT_STUB(__imp__XamUserAddRecentPlayer);
REX_EXPORT_STUB(__imp__XamUserAllowedToPostToSocialNetwork);
REX_EXPORT_STUB(__imp__XamUserCreatePlayerEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreateTitlesPlayedEnumerator);
REX_EXPORT_STUB(__imp__XamUserFlushLogonQueue);
REX_EXPORT_STUB(__imp__XamUserGetAge);
REX_EXPORT_STUB(__imp__XamUserGetAgeGroup);
REX_EXPORT_STUB(__imp__XamUserGetCachedUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetDeviceId);
REX_EXPORT_STUB(__imp__XamUserGetIndexFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetMembershipTierFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineCountryFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineLanguageFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineXUIDFromOfflineXUID);
REX_EXPORT_STUB(__imp__XamUserGetReportingInfo);
REX_EXPORT_STUB(__imp__XamUserGetRequestedUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserGetUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetUserFlagsFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetUserTenure);
REX_EXPORT_STUB(__imp__XamUserGetUsersMissingAvatars);
REX_EXPORT_STUB(__imp__XamUserGetXUIDForTFA);
REX_EXPORT_STUB(__imp__XamUserInvalidateProfileSetting);
REX_EXPORT_STUB(__imp__XamUserIsGuest);
REX_EXPORT_STUB(__imp__XamUserIsLogonPreviewModeEnabled);
REX_EXPORT_STUB(__imp__XamUserIsParentalControlled);
REX_EXPORT_STUB(__imp__XamUserIsPartial);
REX_EXPORT_STUB(__imp__XamUserIsPartialProfile);
REX_EXPORT_STUB(__imp__XamUserIsUnsafeProgrammingAllowed);
REX_EXPORT_STUB(__imp__XamUserLockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserLogon);
REX_EXPORT_STUB(__imp__XamUserLogonEx);
REX_EXPORT_STUB(__imp__XamUserLookupDevice);
REX_EXPORT_STUB(__imp__XamUserNuiBind);
REX_EXPORT_STUB(__imp__XamUserNuiEnableBiometric);
REX_EXPORT_STUB(__imp__XamUserNuiGetEnrollmentIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForBind);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForSignin);
REX_EXPORT_STUB(__imp__XamUserNuiIsBiometricEnabled);
REX_EXPORT_STUB(__imp__XamUserNuiUnbind);
REX_EXPORT_STUB(__imp__XamUserOverrideBindingCallbacks);
REX_EXPORT_STUB(__imp__XamUserOverrideDeviceBindings);
REX_EXPORT_STUB(__imp__XamUserOverrideGlobalState);
REX_EXPORT_STUB(__imp__XamUserOverrideUserInfo);
REX_EXPORT_STUB(__imp__XamUserPrefetchProfileSettings);
REX_EXPORT(__imp__XamUserProfileSync, rex::kernel::xam::XamUserProfileSync_entry)
REX_EXPORT_STUB(__imp__XamUserReadUserPreference);
REX_EXPORT_STUB(__imp__XamUserResetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserUnlockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserUpdateRecentPlayer);
REX_EXPORT(__imp__XamUserValidateAvatarManifest,
           rex::kernel::xam::XamUserValidateAvatarManifest_entry)
REX_EXPORT_STUB(__imp__XamUserWriteUserPreference);
REX_EXPORT_STUB(__imp__XamVerifyPasscode);
