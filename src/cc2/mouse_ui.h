#pragma once
#include <cstdint>
#include <rex/ppc/context.h>
namespace rex::input { struct X_INPUT_GAMEPAD; }
namespace rex::ui { class Window; }

// Host mouse driven into the game's own GUI pointer: the hand cursor follows the mouse, hover uses the GUI's
// cursor-over path, left click presses A and right click presses B through GUIINPUT.
namespace cc2::mouse {
void ObservePad(uint32_t user, const rex::input::X_INPUT_GAMEPAD* pad);
void Initialize(rex::ui::Window* window);
void Shutdown();
void Tick(PPCContext& ctx, uint8_t* base);
// Draws the hand as the game's own 3D pointer (GRES:common/oa/pointer0) on a GUI layer of ours; run after Tick.
void GloveTick(PPCContext& ctx, uint8_t* base);
}
