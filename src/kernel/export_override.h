#pragma once

namespace rex {
class Runtime;
}

namespace cc2 {

// Make every REX_EXPORT in the exe the one the runtime hands out for
// ordinal (GetProcAddressByOrdinal) lookups too. Call once, before any
// guest code runs.
void PromoteTitleExports();

// Log whether each title export, import hook and the indirect-call resolver is the one the game reaches.
void LogOverrideCheck(rex::Runtime* runtime);

}  // namespace cc2
