#pragma once
#include <cstdint>
#include <rex/input/input.h>
#include <rex/ppc/context.h>

namespace rex::ui { class Window; }

// Free camera for single-player courses: the settings row turns it on, then the mouse and WASD (or the sticks)
// fly the gameplay camera while the pad input the course would see is swallowed. The pause menu still opens.
namespace cc2::freecam {
void Initialize(rex::ui::Window* window);
void Shutdown();
// Once per frame on the guest thread; `course` and `paused` come from the menu Lua, `enabled` (free camera) and
// `close_up` (close-up follow camera) from the settings. The free camera wins when both are on.
void Tick(bool enabled, bool close_up, bool course, bool paused);
void FilterInput(uint32_t user, rex::input::X_INPUT_GAMEPAD* pad);
bool Active();
uint32_t OverriddenFrustum();  // the free or close-up camera's frustum, 0 when neither is on
// Writes the owned pose into a camera actor; the camera hooks call it.
void ApplyPose(uint32_t camera);
}  // namespace cc2::freecam
