#pragma once

namespace rex {
class Runtime;
namespace system {
class KernelState;
}
}  // namespace rex

namespace cc2 {

// Mirrors <game_data_root>/DLC (or fresh_save) into user_data_root without overwriting.
// Runs at kernel init, before the runtime has a title id.
void SeedUserData(rex::Runtime* runtime);

// Writes a .header for every common-content package that lacks one. Needs the
// loaded module's title id, so it runs from OnPreLaunchModule.
void EnsureContentHeaders(rex::Runtime* runtime, rex::system::KernelState* kernel_state);

}  // namespace cc2
