// Minimal-render file cull.
//
// Theme content is precache-loaded in bulk before the per-model loads the
// guest-side gates in sentient.cpp intercept, so the file is the only choke
// point that covers every loader. Remount the game device behind a wrapper
// that fails matching opens as file-not-found, a failure mode the engine
// already handles.

#include "vfs_cull.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>

namespace cc2 {

namespace {

// Must match the mount Runtime::SetupVfs uses; game: and d: link to it.
constexpr const char* kGameMount = "\\Device\\Harddisk0\\Partition1";

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

class CullingHostPathDevice : public rex::filesystem::HostPathDevice {
 public:
  CullingHostPathDevice(const std::string_view mount_path, const std::filesystem::path& host_path,
                        bool read_only, std::vector<std::string> denied)
      : HostPathDevice(mount_path, host_path, read_only), denied_(std::move(denied)) {}

  rex::filesystem::Entry* ResolvePath(const std::string_view path) override {
    const std::string lower = ToLower(std::string(path));
    for (const std::string& prefix : denied_) {
      if (lower.find(prefix) != std::string::npos) {
        return nullptr;
      }
    }
    return HostPathDevice::ResolvePath(path);
  }

 private:
  std::vector<std::string> denied_;
};

std::vector<std::string> ParseList(const std::string& csv) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= csv.size()) {
    size_t comma = csv.find(',', start);
    if (comma == std::string::npos) comma = csv.size();
    std::string item = ToLower(csv.substr(start, comma - start));
    item.erase(0, item.find_first_not_of(" \t"));
    if (auto end = item.find_last_not_of(" \t"); end != std::string::npos) item.erase(end + 1);
    if (!item.empty()) out.push_back(item);
    start = comma + 1;
  }
  return out;
}

}  // namespace

void InstallVfsCull(rex::Runtime* runtime, const std::string& csv_prefixes) {
  auto denied = ParseList(csv_prefixes);
  if (denied.empty() || !runtime || !runtime->file_system()) {
    return;
  }
  auto* fs = runtime->file_system();
  if (!fs->UnregisterDevice(kGameMount)) {
    REXLOG_WARN("[cull] game device not mounted at {}, cull not installed", kGameMount);
    return;
  }
  auto device = std::make_unique<CullingHostPathDevice>(
      kGameMount, std::filesystem::absolute(runtime->game_data_root()), /*read_only=*/true,
      std::move(denied));
  if (!device->Initialize() || !fs->RegisterDevice(std::move(device))) {
    REXLOG_ERROR("[cull] failed to remount the game device; the title has no game data now");
    return;
  }
  REXLOG_INFO("[cull] game device remounted with {} denied path fragments", denied.size());
}

}  // namespace cc2
