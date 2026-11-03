#pragma once

namespace rex { class Runtime; }

// JMprofile lists a game from the SDK's unlock file plus <titleid>.catalog.toml, the game tile <TITLEID>.png and
// the achievement icons under <TITLEID>\, all written here from the title's XDBF.
namespace cc2::achievements {
void WriteCatalog(rex::Runtime* runtime);
}  // namespace cc2::achievements
