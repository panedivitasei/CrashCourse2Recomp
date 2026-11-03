// Shipped userdata baseline.
//
// The Hong Kong DLC lives in the title's DLC tree (fresh_save in some archive
// layouts) and must be present under userdata before the additional-content flow
// enumerates, or the boot hangs at "Please Wait" trying to download it. Existing
// saves win.
//
// The runtime's enumerator reads a per-package .header and, when the file is
// missing, hands the title an uninitialized record (its not-found check is an
// NTSTATUS mask). The DLC tree ships no headers, so synthesize one per package.

#include "content_seed.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

namespace cc2 {

namespace fs = std::filesystem;
using rex::system::xam::XCONTENT_AGGREGATE_DATA;
using rex::system::XContentType;

namespace {

bool IsHex8(const std::string& s) {
  if (s.size() != 8) return false;
  for (char c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

}  // namespace

void EnsureContentHeaders(rex::Runtime* runtime, rex::system::KernelState* kernel_state) {
  const fs::path user_root = runtime->user_data_root();
  if (user_root.empty()) return;
  char title[16];
  std::snprintf(title, sizeof(title), "%08X", kernel_state->title_id());
  const fs::path common = user_root / "0000000000000000" / title;
  std::error_code ec;
  if (!fs::is_directory(common, ec)) return;

  auto* content_manager = kernel_state->content_manager();
  int written = 0;
  for (const auto& type_dir : fs::directory_iterator(common, ec)) {
    const std::string type_name = type_dir.path().filename().string();
    if (!type_dir.is_directory(ec) || !IsHex8(type_name)) continue;
    const auto content_type =
        static_cast<XContentType>(std::stoul(type_name, nullptr, 16));
    for (const auto& pkg : fs::directory_iterator(type_dir.path(), ec)) {
      if (!pkg.is_directory(ec)) continue;
      const std::string name = pkg.path().filename().string();
      const fs::path header = common / "Headers" / type_name / (name + ".header");
      if (fs::exists(header, ec)) continue;
      XCONTENT_AGGREGATE_DATA data;
      data.device_id = 1;  // HDD
      data.content_type = content_type;
      data.set_display_name(std::u16string(name.begin(), name.end()));
      data.set_file_name(name);
      data.title_id = kernel_state->title_id();
      data.xuid = 0;
      if (content_manager->WriteContentHeaderFile(0, data) == 0) {
        ++written;
      } else {
        REXLOG_WARN("[seed] header write failed for {}/{}", type_name, name);
      }
    }
  }
  if (written) REXLOG_INFO("[seed] {} content header(s) synthesized", written);
}

void SeedUserData(rex::Runtime* runtime) {
  const fs::path user_root = runtime->user_data_root();
  if (user_root.empty()) return;
  std::error_code ec;
  fs::path seed = runtime->game_data_root() / "DLC";
  if (!fs::is_directory(seed, ec)) seed = runtime->game_data_root() / "fresh_save";
  if (fs::is_directory(seed, ec)) {
    fs::create_directories(user_root, ec);
    fs::copy(seed, user_root, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    if (ec) {
      REXLOG_WARN("[seed] {} copy failed: {}", seed.filename().string(), ec.message());
    } else {
      REXLOG_INFO("[seed] {} mirrored into {}", seed.filename().string(), user_root.string());
    }
  }
}

}  // namespace cc2
