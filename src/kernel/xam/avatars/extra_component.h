// Extra avatar component: a raw STRB asset (DCC2's data/art/_avatar/ai-assets/*.bin special AI looks) appended to
// one XamAvatarGetAssets build.

#ifndef REX_AVATARS_EXTRA_COMPONENT_H_
#define REX_AVATARS_EXTRA_COMPONENT_H_

#include <memory>
#include <utility>
#include <vector>

#include "guest_asset.h"
#include "model.h"

#include "xe_compat.h"

namespace rex {
namespace avatars {

// Queues a STRB blob for the calling thread's next LoadAssetsToGuest build; nullptr/0 clears it. Per-thread because
// avatar builds run concurrently on several guest threads.
void SetPendingExtraComponentStrb(const uint8_t* data, size_t size);

// Called from LoadAssetsToGuest: decodes the queued STRB (if any) and appends it to the component list. Returns the
// appended component's index (serialize it category-neutral so its own textures render untinted), or -1 when nothing
// was queued or the decode failed.
int AppendPendingExtraComponent(
    std::vector<std::pair<X_AVATAR_COMPONENT_INFO, std::shared_ptr<Model>>>& source_components,
    ModelLoadOptions model_load_options);

}  // namespace avatars
}  // namespace rex

#endif  // REX_AVATARS_EXTRA_COMPONENT_H_
