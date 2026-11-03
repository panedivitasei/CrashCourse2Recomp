#pragma once
#include <string>

// The player's avatar is lit too brightly on Hong Kong courses; there, avatar lighting is scaled by
// cc2_hongkong_avatar_light.
namespace cc2::avatar_light {
void SceneLoaded(const std::string& path);  // SCENE::Load, before the load
}  // namespace cc2::avatar_light
