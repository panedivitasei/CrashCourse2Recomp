#pragma once

#include <string>

namespace rex {
class Runtime;
}

namespace cc2 {

// Remount the game device so opens whose path contains any comma-separated
// fragment in `csv_prefixes` fail as not-found. No-op on an empty list.
void InstallVfsCull(rex::Runtime* runtime, const std::string& csv_prefixes);

}  // namespace cc2
