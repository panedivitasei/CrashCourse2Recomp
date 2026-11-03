#pragma once

#include <cstdint>
#include <string_view>
#include <rex/input/input.h>
#include <rex/ppc/context.h>

namespace rex::ui { class Window; }

namespace cc2::display {
void Initialize(rex::ui::Window* window);
void Shutdown();
void Tick(PPCContext& ctx, uint8_t* base);
void FilterInput(uint32_t user, rex::input::X_INPUT_GAMEPAD* pad);
// Mouse support for settings rows and their native bars.
void MouseActive(bool active);
void MouseMoved();  // one hover selection after a pointer move reaches the GUI
bool MouseSlider(PPCContext& ctx, uint8_t* base, int row, float fraction);  // settings rows 1-14
void MouseSliderEnd();
bool MouseClick(int row);  // display row 1-11 was hit by the pointer; steps that setting
bool MainMenuPointer(PPCContext& ctx, uint8_t* base, int button, bool click);
bool CoursePointer(PPCContext& ctx, uint8_t* base, float x, float y, bool click);
bool ShopPointer(PPCContext& ctx, uint8_t* base, float x, float y, std::string_view tab,
                 uint32_t control, bool click);
bool ShopScroll(PPCContext& ctx, uint8_t* base, int rows);
int FocusRow();     // the menu state CC2DisplayTick last returned, for the mouse trace
void MouseWheel(PPCContext& ctx, uint8_t* base, int steps);
void MouseScrollTo(float value);
int ExpandedMenuButton();
void ScrollText(PPCContext& ctx, uint8_t* base, float value, bool relative);
int ResolutionScale();
int ShadowScale();  // course shadow map percent, the Shadow Resolution row
int FrameLimit();
bool FpsCounter();  // the FPS Counter row; the renderer draws the counter while it is on
float AspectRatio();
}
