// Shared avatar data folder: the manifest the Avatar Editor saves and the
// gamer pictures beside it. Same resolution as the manifest path in
// xam_avatar.cpp (avatar_data_dir cvar, else <user_data_root>/avatars).

#include <filesystem>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include "cc2/kernel/xam/xam_avatar.h"
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

namespace rex::kernel::xam {
// Defined in this namespace by xam_avatar.cpp.
REXCVAR_DECLARE(std::string, avatar_data_dir);
}  // namespace rex::kernel::xam

namespace cc2::kernel::xam {

using rex::kernel::xam::FLAGS_avatar_data_dir_storage_;

std::string AvatarDataDir() {
  std::string dir = REXCVAR_GET(avatar_data_dir);
  if (dir.empty()) {
    const auto& root = REX_KERNEL_STATE()->emulator()->user_data_root();
    if (!root.empty()) {
      dir = (root / "avatars").string();
    }
  }
  if (dir.empty()) {
    dir = "avatars";
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

}  // namespace cc2::kernel::xam
