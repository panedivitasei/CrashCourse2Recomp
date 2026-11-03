#include "cc2/achievement_catalog.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

namespace cc2::achievements {
namespace {

std::string TomlString(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

void WriteBlock(const std::filesystem::path& path, const rex::system::util::XdbfBlock& block) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(block.buffer), std::streamsize(block.size));
}

}  // namespace

void WriteCatalog(rex::Runtime* runtime) {
  auto* kernel = runtime ? runtime->kernel_state() : nullptr;
  if (!kernel || runtime->user_data_root().empty()) return;
  const auto db = kernel->title_xdbf();
  const auto entries = kernel->loaded_achievements();
  if (!db.is_valid() || entries.empty()) return;
  const auto dir = runtime->user_data_root() / "achievements";
  const auto title = fmt::format("{:08X}", kernel->title_id());
  std::error_code ec;
  std::filesystem::create_directories(dir / title, ec);
  std::ofstream out(dir / (title + ".catalog.toml"), std::ios::binary);
  out << "title = " << TomlString(db.title().empty() ? "Crash Course 2" : db.title()) << "\n";
  for (const auto& entry : entries) {
    out << "\n[[achievements]]\n"
        << "id = " << entry.id << "\n"
        << "label = " << TomlString(entry.label) << "\n"
        << "description = " << TomlString(entry.description) << "\n"
        << "unachieved_description = " << TomlString(entry.unachieved_description) << "\n"
        << "gamerscore = " << entry.gamerscore << "\n"
        << "flags = " << entry.flags << "\n";
    if (const auto image = db.GetEntry(rex::system::util::XdbfSection::kImage, entry.image_id); image) {
      WriteBlock(dir / title / fmt::format("{}.png", entry.id), image);
    }
  }
  if (const auto icon = db.icon(); icon) WriteBlock(dir / (title + ".png"), icon);
  REXLOG_DEBUG("[achievements] wrote the catalog of {} entries to {}", entries.size(), dir.string());
}

}  // namespace cc2::achievements
