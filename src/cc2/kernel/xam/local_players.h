#pragma once
#include <cstdint>

// Local players 2-4: a controller assigned to user index 1-3 is a profile signed in locally.
namespace cc2::kernel::xam {
bool LocalPlayer(uint32_t user_index);           // users 1-3 with a connected controller
uint64_t LocalPlayerXuid(uint32_t user_index);   // offline-format XUID per local player
uint32_t SignedInUserMask();                     // XN_SYS_SIGNINCHANGED parameter: bit per signed-in user
void PollLocalPlayers();                         // per frame: sign-in changes and the Guide sign-in closing
}  // namespace cc2::kernel::xam
