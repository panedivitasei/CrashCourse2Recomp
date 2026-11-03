#include "cc2/keyboard_bindings.h"
#include <rex/cvar.h>
#include <rex/logging.h>

namespace cc2::keyboard {
void Initialize() {
  Bindings original;
  for (const auto& entry : rex::cvar::GetRegistry())
    if (entry.category == "Input/Keybinds/Controller" && entry.type == rex::cvar::FlagType::String)
      original.emplace(entry.name, entry.getter());
  const auto prepared = Prepare(original);
  for (const auto& [name, value] : prepared) {
    if (original[name] == value) continue;
    if (rex::cvar::SetFlagByName(name, value)) REXLOG_DEBUG("[keyboard] {} = {}", name, value);
  }
}
}  // namespace cc2::keyboard
