#pragma once
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace cc2::keyboard {
using Bindings = std::map<std::string, std::string>;
inline std::vector<std::string> Tokens(std::string_view value) {
  std::vector<std::string> result;
  while (!value.empty()) {
    const auto comma = value.find(',');
    auto token = value.substr(0, comma);
    const auto begin = token.find_first_not_of(" ");
    if (begin != std::string_view::npos)
      result.emplace_back(token.substr(begin, token.find_last_not_of(" ") - begin + 1));
    if (comma == std::string_view::npos) break;
    value.remove_prefix(comma + 1);
  }
  return result;
}
inline Bindings Prepare(Bindings bindings) {
  auto& start = bindings["keybind_start"];
  bool escape = false;
  for (const auto& token : Tokens(start)) escape |= token == "Escape";
  if (!escape) start += (start.empty() ? "" : ",") + std::string("Escape");
  std::set<std::string> explicit_shift;
  for (const auto& [name, value] : bindings)
    for (const auto& token : Tokens(value))
      if (token.starts_with("Shift+")) explicit_shift.insert(token);
  for (auto& [name, value] : bindings) {
    std::vector<std::string> output;
    auto append = [&](const std::string& token) {
      for (const auto& existing : output) if (existing == token) return;
      output.push_back(token);
    };
    for (const auto& token : Tokens(value)) {
      // The SDK matches modifier masks exactly, including the modifier key itself.
      if (token == "Shift") { append("Shift+Shift"); continue; }
      append(token);
      if (token.find('+') == std::string::npos && token != "Ctrl" &&
          token != "Control" && token != "Alt" && !explicit_shift.contains("Shift+" + token))
        append("Shift+" + token);
    }
    value.clear();
    for (const auto& token : output) { if (!value.empty()) value += ','; value += token; }
  }
  return bindings;
}
void Initialize();
}  // namespace cc2::keyboard
