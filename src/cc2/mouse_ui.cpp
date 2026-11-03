#include "cc2/mouse_ui.h"
#include "cc2/display_settings.h"
#include "cc2/lobby.h"
#include "gpu/gpu.h"
#include <rex/input/input.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/hook.h>
#include <windows.h>
#include <commctrl.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>
#include <imgui.h>
#include <rexglue_guide/guide.h>

REXCVAR_DEFINE_BOOL(cc2_mouse, true, "Input", "Drive the game's GUI pointer with the mouse; left click is A, right click is B");
REXCVAR_DEFINE_INT32(cc2_mouse_move_hz, 0, "Input", "Cap on pointer-move events sent to the GUI per second; 0 = every frame");
REXCVAR_DEFINE_STRING(cc2_mouse_cursors, "menus,pause,guimessages", "Input",
                      "Comma-separated GUI cursor names the mouse drives");

// GUIINPUT (TU1): the pointer move and button entry points the pad's pointer uses, called here with the mouse.
// Both take a 40-byte event by value in r4..r8, two dwords per register: {?,?} {user,device} {?,?} {x,y} {bound,?}.
extern "C" REX_FUNC(sub_82AA9078);  // GUIINPUT::OnJoystickPointerMove(this, event)
extern "C" REX_FUNC(sub_82AA9EF0);  // GUIINPUT::OnJoystickButtonDown(this, event with {button,?} in r6)
extern "C" REX_FUNC(sub_82AA9C30);  // GUIINPUT::OnJoystickButtonUp(this, event)
extern "C" REX_FUNC(sub_82ABA8B0);  // GUICONTROL::GetAbsEnabled
extern "C" REX_FUNC(sub_82ABA830);  // GUICONTROL::GetAbsVisible
extern "C" REX_FUNC(__imp__sub_82A22F60);
extern "C" REX_FUNC(sub_82A22F60);  // GUICLICKABLE::ContainsPoint
extern "C" REX_FUNC(sub_82ABB300);  // GUICONTROL::AbsToLocalPoint
extern "C" REX_FUNC(sub_82A37E70);  // CURSOR::UpdateMouseOver(this, point, force)
extern "C" REX_FUNC(sub_82B9D808);  // GUICLICKABLEMASTER::DispatchKeyBinding
extern "C" REX_FUNC(sub_822AB270);  // push GUI input owner
extern "C" REX_FUNC(__imp__sub_82AA94A0);  // GUIINPUT::ForceCursorOnClickable
extern "C" REX_FUNC(__imp__sub_82A22E38);  // GUICLICKABLE::OnMouseOut
extern "C" REX_FUNC(__imp__sub_82A22D38);  // GUICLICKABLE::OnMouseOver
extern "C" REX_FUNC(sub_82B9EF58);  // GUISCROLLBAR::SetValue
extern "C" REX_FUNC(sub_82A22648);  // GUICLICKABLE::SetCursor(this, cursor), recursive over a group

REXCVAR_DEFINE_BOOL(cc2_native_glove, true, "Input",
                    "Draw the mouse hand as the game's own 3D pointer instead of the baked sprite");
REXCVAR_DEFINE_DOUBLE(cc2_glove_offset_x, 0.0, "Input", "Native glove offset from the mouse, GUI units");
REXCVAR_DEFINE_DOUBLE(cc2_glove_offset_y, 0.0, "Input", "Native glove offset from the mouse, GUI units");
REXCVAR_DEFINE_DOUBLE(cc2_glove_size, 0.0, "Input", "Native glove scale (0 keeps the game's own size)");
REXCVAR_DEFINE_STRING(cc2_glove_layer, "pause:title;main;screens;popups", "Input",
                      "GUI layers for the native glove, first usable one wins; layer:control needs that control up");
REXCVAR_DEFINE_DOUBLE(cc2_glove_angle, -135.0, "Input", "Native glove rotation in degrees, clockwise; 0 points right");
REXCVAR_DEFINE_DOUBLE(cc2_glove_tip, 0.058, "Input", "Distance from the native glove's pivot to its fingertip, GUI units");
REXCVAR_DEFINE_INT32(cc2_glove_zsort, 255, "Input", "Absolute z-sort of the native glove; the GUI draws nothing above 255");

namespace cc2::mouse {
namespace {

constexpr uint32_t kGuiInput = 0x82EDC358;    // GUIINPUT* global
constexpr uint32_t kGuiStates = 0x82E993CC;   // pointer to the per-user GUI state table, entry (user + 13)
constexpr uint32_t kDevice = 1;               // the engine's mouse device slot; its mask bit also gates the decal
constexpr uint32_t kOffsetX = 0x82EDC334;     // INPUT's normalised-to-GUI transform, read live
constexpr uint32_t kOffsetY = 0x82EDC338;
constexpr uint32_t kAspectShift = 0x82EDC33C;
constexpr uint32_t kScaleX = 0x82D9F100;
constexpr uint32_t kScaleY = 0x82D9F104;
constexpr uint32_t kAspect = 0x82D9F108;
constexpr uint32_t kButtonA = 38;
constexpr uint32_t kButtonB = 39;

struct ButtonEvent {
  uint32_t button;
  bool down;
  int32_t x, y;
};

std::atomic<rex::ui::Window*> g_window{nullptr};
std::atomic<bool> g_menu_cursor{false};  // the guest says a menu cursor is active; the UI thread reads it per event
std::atomic<bool> g_mouse_active{false};
std::atomic<bool> g_mouse_enabled{true};
constexpr UINT_PTR kPointerSubclass = 0xCC2;

// Both SDL events and ScreenToClient supply physical client pixels.
bool Normalize(rex::ui::Window* w, int32_t mx, int32_t my, float& nx, float& ny) {
  const uint32_t pw = w->GetActualPhysicalWidth(), ph = w->GetActualPhysicalHeight();
  if (!pw || !ph) return false;
  const float px = float(mx), py = float(my);
  const float aspect = cc2::display::AspectRatio();
  float fw = float(pw), fh = float(ph);
  if (fw / fh > aspect) fw = fh * aspect;
  else fh = fw / aspect;
  const float fx = (float(pw) - fw) * 0.5f, fy = (float(ph) - fh) * 0.5f;
  nx = (px - fx) / fw;
  ny = (py - fy) / fh;
  return nx >= 0.0f && nx <= 1.0f && ny >= 0.0f && ny <= 1.0f;
}

// True while the Guide or an ImGui window has the mouse. UI thread.
bool OverlayOwnsMouse() {
  if (rexglue_guide::IsOpen()) return true;
  return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
}

// Cursor ownership follows window focus; input mode controls only the rendered hand.
bool PresentPointer(rex::ui::Window* w, int32_t mx, int32_t my) {
  float nx = 0, ny = 0;
  const bool inside = Normalize(w, mx, my, nx, ny);
  const HWND hwnd = static_cast<HWND>(w->GetNativeWindowHandle());
  POINT point{};
  const bool over_window = GetCursorPos(&point) && WindowFromPoint(point) == hwnd;
  const bool owns_cursor =
      over_window && GetForegroundWindow() == hwnd && g_mouse_enabled.load() && !OverlayOwnsMouse();
  cc2::gpu::SetPointer(nx, ny, owns_cursor && inside && g_menu_cursor.load() && g_mouse_active.load());
  // Keep SDL's visibility count enabled; the window procedure owns the cursor image.
  if (w->GetCursorVisibility() != rex::ui::Window::CursorVisibility::kVisible)
    w->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
  SetCursor(owns_cursor ? nullptr : LoadCursor(nullptr, IDC_ARROW));
  return owns_cursor;
}

bool PointerPosition(rex::ui::Window* w, int32_t& x, int32_t& y) {
  POINT point{};
  if (!GetCursorPos(&point) || !ScreenToClient(static_cast<HWND>(w->GetNativeWindowHandle()), &point)) return false;
  x = point.x;
  y = point.y;
  return true;
}

void RefreshPointerNow(rex::ui::Window* w);

class Listener : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (OverlayOwnsMouse()) {
      if (auto* w = g_window.load()) PresentPointer(w, e.x(), e.y());
      return;
    }
    {
      std::lock_guard lock(mutex);
      if (x != e.x() || y != e.y()) {
        ++motion_serial;
        g_mouse_active.store(true);
      }
      x = e.x();
      y = e.y();
      moved = true;
    }
    if (auto* w = g_window.load()) PresentPointer(w, e.x(), e.y());
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (OverlayOwnsMouse()) return;
    g_mouse_active.store(true);
    if (auto* w = g_window.load()) PresentPointer(w, e.x(), e.y());
    Push(e, true);
    if (e.button() == rex::ui::MouseEvent::Button::kLeft && g_menu_cursor.load() && !captured) {
      if (auto* w = g_window.load()) { w->CaptureMouse(); captured = true; }
    }
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    Push(e, false);
    if (e.button() == rex::ui::MouseEvent::Button::kLeft) ReleaseCapture();
  }
  void OnGotFocus(rex::ui::UISetupEvent&) override {
    if (auto* w = g_window.load()) RefreshPointerNow(w);
  }
  void OnKeyDown(rex::ui::KeyEvent&) override {
    g_mouse_active.store(false);
    if (auto* w = g_window.load()) RefreshPointerNow(w);
  }
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    ReleaseCapture();
    if (auto* w = g_window.load()) RefreshPointerNow(w);
    std::lock_guard lock(mutex);
    buttons.clear();
    wheel = 0;
    moved = false;
    buttons.push_back({kButtonA, false, x, y});
    buttons.push_back({kButtonB, false, x, y});
  }
  void ReleaseCapture() {
    if (!captured) return;
    captured = false;
    if (auto* w = g_window.load()) w->ReleaseMouse();
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (OverlayOwnsMouse()) return;
    g_mouse_active.store(true);
    if (auto* w = g_window.load()) PresentPointer(w, e.x(), e.y());
    std::lock_guard lock(mutex);
    wheel += e.scroll_y();
  }

  std::mutex mutex;
  int32_t x = 0, y = 0;
  int32_t wheel = 0;  // scroll units, 120 per detent
  bool moved = false;
  uint64_t motion_serial = 0;
  std::deque<ButtonEvent> buttons;

 private:
  bool captured = false;
  void Push(rex::ui::MouseEvent& e, bool down) {
    uint32_t button = 0;
    if (e.button() == rex::ui::MouseEvent::Button::kLeft) button = kButtonA;
    else if (e.button() == rex::ui::MouseEvent::Button::kRight) button = kButtonB;
    if (!button) return;
    std::lock_guard lock(mutex);
    x = e.x();
    y = e.y();
    moved = true;
    if (buttons.size() < 16) buttons.push_back({button, down, e.x(), e.y()});
  }
};

Listener g_listener;
std::atomic<bool> g_pad_active{false};
bool g_hand_published = false;   // the last hand_visible Tick handed to the UI thread
float g_hand_gx = 0, g_hand_gy = 0;  // the mouse in GUI space, from the last Tick
bool g_down[2] = {false, false};  // A and B presses the GUI has seen without their release
bool g_swallowed = false;  // a left press handled by the host, paired with its release
struct ScrollDrag {
  uint32_t control = 0, cursor = 0, id = 0, generation = 0;
  float offset = 0;
  int setting_row = 0;
  uint32_t owner = 0;
};
ScrollDrag g_drag;
void EndDrag() {
  if (g_drag.setting_row) cc2::display::MouseSliderEnd();
  g_drag = {};
}
int32_t g_sent_x = INT32_MIN, g_sent_y = INT32_MIN;  // window position of the last move the GUI was sent

uint32_t Read32(uint32_t address) {
  return *REX_KERNEL_MEMORY()->TranslateVirtual<const rex::be<uint32_t>*>(address);
}
float ReadFloat(uint32_t address) {
  return *REX_KERNEL_MEMORY()->TranslateVirtual<const rex::be<float>*>(address);
}
void Write32(uint32_t address, uint32_t value) {
  *REX_KERNEL_MEMORY()->TranslateVirtual<rex::be<uint32_t>*>(address) = value;
}
uint8_t Read8(uint32_t address) { return *REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address); }
uint32_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

// The GUI state of user 0: cursor mask at +92, pointer position at +196/+200.
uint32_t GuiState() {
  const uint32_t table = Read32(kGuiStates);
  return table ? Read32(table + 4 * 13) : 0;
}

// The cursor object's name (this+12, the one its own error messages print): the front-end and pause
// cursors take the mouse; the in-level "game" cursor and the boot-time defaults leave the pointer hidden.
bool MenuCursor(uint32_t cursor) {
  static std::string list_source;
  static std::vector<std::string> names;
  const uint32_t name = cursor ? Read32(cursor + 12) : 0;
  if (!name) return false;
  const std::string list = REXCVAR_GET(cc2_mouse_cursors);
  if (list != list_source) {
    list_source = list;
    names.clear();
    for (size_t at = 0; at <= list.size();) {
      const size_t comma = std::min(list.find(',', at), list.size());
      if (comma > at) names.push_back(list.substr(at, comma - at));
      at = comma + 1;
    }
  }
  const char* text = REX_KERNEL_MEMORY()->TranslateVirtual<const char*>(name);
  const std::string_view label(text, strnlen(text, 32));
  bool menu = false;
  for (const auto& n : names) menu = menu || label == n;
  return menu;
}

// Two dwords packed the way the callee reads them back from a 64-bit spill: high word first.
uint64_t Pair(uint32_t high, uint32_t low) { return (uint64_t(high) << 32) | low; }

// The rendered pointer and guest hit tests share the same physical-pixel normalization.
bool ToGui(rex::ui::Window* w, int32_t mx, int32_t my, float& gx, float& gy, float& nx_out, float& ny_out) {
  float nx = 0, ny = 0;
  const bool inside = Normalize(w, mx, my, nx, ny);
  nx_out = nx;
  ny_out = ny;
  gx = (nx - ReadFloat(kOffsetX)) / ReadFloat(kScaleX);
  gy = (ny - ReadFloat(kOffsetY)) / ReadFloat(kScaleY);
  const float a = ReadFloat(kAspect);
  if (a != 1.0f) gx = gx * a - ReadFloat(kAspectShift);
  return inside && std::isfinite(gx) && std::isfinite(gy);
}

void RefreshPointerNow(rex::ui::Window* w) {
  int32_t x = 0, y = 0;
  if (!PointerPosition(w, x, y)) return;
  PresentPointer(w, x, y);
}

// WM_SETCURSOR decides the client cursor before Windows applies the window class cursor.
LRESULT CALLBACK PointerWindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
                                   UINT_PTR id, DWORD_PTR) {
  auto* w = g_window.load();
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(hwnd, PointerWindowProc, id);
    return DefSubclassProc(hwnd, message, wp, lp);
  }
  if (!w || w->GetNativeWindowHandle() != hwnd) return DefSubclassProc(hwnd, message, wp, lp);
  if (message == WM_SETCURSOR) {
    int32_t x = 0, y = 0;
    if (LOWORD(lp) == HTCLIENT && PointerPosition(w, x, y) && PresentPointer(w, x, y)) return TRUE;
    cc2::gpu::SetPointer(0, 0, false);
  }
  const LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
  if (g_window.load() != w) return result;
  switch (message) {
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&tracking);
      RefreshPointerNow(w);
      break;
    }
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_MOUSELEAVE:
    case WM_CAPTURECHANGED:
    case WM_SIZE:
      RefreshPointerNow(w);
      break;
  }
  return result;
}

void RefreshPointer(rex::ui::Window* w) {
  w->app_context().CallInUIThreadDeferred([w] {
    if (g_window.load() == w) RefreshPointerNow(w);
  });
}

std::string_view ControlName(uint32_t control) {
  const uint32_t name = Read32(control + 12);
  if (!name) return {};
  const char* text = REX_KERNEL_MEMORY()->TranslateVirtual<const char*>(name);
  return {text, strnlen(text, 128)};
}

bool MainMenuControl(uint32_t control) {
  if (!control) return false;
  const uint32_t parent=Read32(control+76);
  return parent && ControlName(Read32(parent+88))=="common/ui/men/main/options";
}
int MainMenuParent(uint32_t control) {
  if (!MainMenuControl(control)) return 0;
  const auto name=ControlName(control);
  return name=="but1" ? 1 : name=="but2" ? 2 : 0;
}

bool ControlEnabled(PPCContext& ctx, uint8_t* base, uint32_t control) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = control;
  sub_82ABA8B0(ctx, base);
  bool enabled = (ctx.r3.u32 & 255) != 0;
  if (enabled) {
    ctx.r3.u64 = control;
    sub_82ABA830(ctx, base);
    enabled = (ctx.r3.u32 & 255) != 0;
  }
  ctx = saved;
  return enabled;
}

bool HitControl(PPCContext& ctx, uint8_t* base, uint32_t control, float x, float y) {
  if (!ControlEnabled(ctx, base, control)) return false;
  const PPCContext saved = ctx;
  ctx.r3.u64 = control;
  ctx.f1.f64 = x;
  ctx.f2.f64 = y;
  sub_82A22F60(ctx, base);
  const bool hit = (ctx.r3.u32 & 255) != 0;
  ctx = saved;
  return hit;
}

struct LocalPoint { float x, y; };

LocalPoint ToLocal(PPCContext& ctx, uint8_t* base, uint32_t control, float x, float y) {
  const PPCContext saved = ctx;
  const uint32_t scratch = (ctx.r1.u32 - 0x100) & ~15u;
  Write32(scratch + 8, Bits(x));
  Write32(scratch + 12, Bits(y));
  ctx.r1.u32 = scratch - 0x80;
  ctx.r3.u64 = scratch;
  ctx.r4.u64 = control;
  ctx.r5.u64 = scratch + 8;
  sub_82ABB300(ctx, base);
  const LocalPoint local{ReadFloat(scratch), ReadFloat(scratch + 4)};
  ctx = saved;
  return local;
}

std::vector<uint32_t> CursorControls(uint32_t cursor) {
  std::vector<uint32_t> controls;
  if (!cursor) return controls;
  const uint32_t first = Read32(cursor + 124), end = Read32(cursor + 128);
  if (!first || end < first || end - first > 4096 * 4) return controls;
  for (uint32_t at = first; at < end; at += 4) controls.push_back(Read32(at));
  return controls;
}

uint32_t ClickableAt(PPCContext& ctx, uint8_t* base, uint32_t cursor, float x, float y) {
  const PPCContext saved = ctx;
  for (const uint32_t c : CursorControls(cursor)) {
    if (!c || Read8(c + 96) || !HitControl(ctx, base, c, x, y)) continue;
    ctx.r3.u64 = c;
    rex::runtime::ResolveIndirectFunction(Read32(Read32(c) + 288))(ctx, base);
    const bool clickable = (ctx.r3.u32 & 255) != 0;
    ctx = saved;
    if (clickable) return c;
  }
  ctx = saved;
  return 0;
}

bool SelectBinding(PPCContext& ctx, uint8_t* base, uint32_t control, uint32_t scope) {
  ctx.r3.u64 = Read32(control + 88);
  ctx.r4.u64 = scope;
  ctx.r5.u64 = 1024;
  ctx.r6.u64 = Pair(Read32(control + 16), Read32(control + 20));
  sub_82B9D808(ctx, base);
  return (ctx.r3.u32 & 255) != 0;
}

// Mouse activation targets one hit-tested control, without the pad's global A binding fallback.
void ClickControl(PPCContext& ctx, uint8_t* base, uint32_t gui, uint32_t cursor,
                  uint32_t control, float x, float y) {
  const PPCContext saved = ctx;
  const uint32_t scratch = (ctx.r1.u32 - 0x100) & ~15u;
  ctx.r1.u32 = scratch - 0x80;
  Write32(cursor + 196, Bits(x));
  Write32(cursor + 200, Bits(y));
  ctx.r3.u64 = cursor;
  ctx.r4.u64 = cursor + 196;
  ctx.r5.u64 = 1;
  sub_82A37E70(ctx, base);
  if (ClickableAt(ctx, base, cursor, x, y) == control) {
    Write32(scratch, 0);
    ctx.r3.u64 = gui + 44;
    ctx.r4.u64 = scratch;
    sub_822AB270(ctx, base);
    // Help & Options uses Select bindings; main-menu buttons use OnClick.
    const bool bound = SelectBinding(ctx, base, control, 1) || SelectBinding(ctx, base, control, 0);
    if (!bound) {
      ctx.r3.u64 = control;
      rex::runtime::ResolveIndirectFunction(Read32(Read32(control) + 268))(ctx, base);
    }
    const uint32_t count = Read32(gui + 60);
    if (count) {
      Write32(gui + 60, count - 1);
      if (count == 1) Write32(gui + 56, 0);
    }
  }
  ctx = saved;
}

int SettingAt(PPCContext& ctx, uint8_t* base, uint32_t cursor, float x, float y) {
  if (cc2::display::FocusRow() < 100) return 0;
  constexpr std::string_view names[] = {"cc2_scale", "cc2_shadow", "cc2_aspect", "cc2_resolution", "cc2_fullscreen", "cc2_limit", "cc2_vsync", "cc2_fps", "cc2_hidehud", "cc2_closeup", "cc2_freecam"};
  for (uint32_t c : CursorControls(cursor)) {
    if (!c || !HitControl(ctx, base, c, x, y)) continue;
    for (int depth = 0; c && depth < 8; ++depth, c = Read32(c + 76)) {
      for (int row = 0; row < 11; ++row) if (ControlName(c) == names[row]) return row + 1;
    }
  }
  return 0;
}

uint32_t SettingBar(uint32_t row) {
  if (!row || Read32(row)!=0x821A95D4) return 0;
  const uint32_t count=Read32(row+872);
  if (count>256) return 0;
  for (uint32_t i=0;i<count;++i) {
    const uint32_t child=Read32(row+876+i*4);
    if (child && Read32(child)==0x821C19DC && ControlName(child)=="bar") return child;
  }
  return 0;
}

int SettingBarRow(uint32_t control) {
  if (cc2::display::FocusRow()<100 || ControlName(control)!="bar") return 0;
  const uint32_t row=Read32(control+76);
  if (!row || ControlName(Read32(row+76))!="sliders") return 0;
  constexpr std::string_view names[]={"music","sfx","vibration","cc2_scale","cc2_shadow","cc2_aspect",
                                      "cc2_resolution","cc2_fullscreen","cc2_limit","cc2_vsync","cc2_fps","cc2_hidehud",
                                      "cc2_closeup","cc2_freecam"};
  for (int i=0;i<14;++i) if (ControlName(row)==names[i]) return i+1;
  return 0;
}

// A control added from Lua is built without a parent and takes the default (menus) cursor, so under the pause cursor
// our settings rows and scrollbar are on no clickable list. The game's own SetCursor moves them next to the shipped rows.
void AdoptSettingRows(PPCContext& ctx, uint8_t* base, uint32_t cursor) {
  if (cc2::display::FocusRow() < 100) return;
  uint32_t group = 0;
  // The shipped rows are on this cursor's list; any of them, or a child of one, leads up to the sliders group.
  for (const uint32_t listed : CursorControls(cursor)) {
    uint32_t c = listed;
    for (int depth = 0; c && depth < 6 && !group; ++depth, c = Read32(c + 76))
      if (Read32(c) == 0x821A95D4 && ControlName(c) == "sliders") group = c;
    if (group) break;
  }
  if (!group) return;
  const uint32_t count = Read32(group + 872);
  if (count > 256) return;
  const PPCContext saved = ctx;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t child = Read32(group + 876 + i * 4);
    if (!child || Read8(child + 96)) continue;
    const uint32_t type = Read32(child);
    if (type != 0x821A95D4 && type != 0x821C2644) continue;  // groups and scrollbars
    const uint32_t old = Read32(child + 140);
    if (old == cursor || !ControlName(child).starts_with("cc2_")) continue;
    ctx.r3.u64 = child;
    ctx.r4.u64 = cursor;
    sub_82A22648(ctx, base);
    ctx = saved;
    REXLOG_INFO("[mouse] settings control '{}' moved from cursor {:#x} to the active cursor {:#x}", ControlName(child),
                old, cursor);
  }
}

bool DragValid(PPCContext& ctx, uint8_t* base, uint32_t cursor) {
  if (!g_drag.control || cursor != g_drag.cursor) return false;
  const auto controls = CursorControls(cursor);
  const uint32_t target=g_drag.setting_row ? g_drag.owner : g_drag.control;
  if (std::find(controls.begin(), controls.end(), target) == controls.end()) return false;
  if (g_drag.setting_row && SettingBar(target)!=g_drag.control) return false;
  const uint32_t type=g_drag.setting_row ? 0x821C19DC : 0x821C2644;
  if (Read32(g_drag.control) != type || Read32(g_drag.control + 16) != g_drag.id ||
      Read32(g_drag.control + 20) != g_drag.generation) return false;
  if (g_drag.setting_row && SettingBarRow(g_drag.control)!=g_drag.setting_row) return false;
  return ControlEnabled(ctx, base, target);
}

void DragScroll(PPCContext& ctx, uint8_t* base, float x, float y) {
  const uint32_t c = g_drag.control;
  const float local = ToLocal(ctx, base, c, x, y).x - g_drag.offset;
  if (g_drag.setting_row) {
    const float width=ReadFloat(Read32(c+88)+64);
    if (!std::isfinite(local) || !std::isfinite(width) || width<=0 ||
        !cc2::display::MouseSlider(ctx, base, g_drag.setting_row, local/width+0.5f)) EndDrag();
    return;
  }
  const float start = ReadFloat(c + 196) + (ReadFloat(c + 172) + ReadFloat(c + 188)) * 0.5f;
  const float end = ReadFloat(c + 200) - (ReadFloat(c + 180) + ReadFloat(c + 188)) * 0.5f;
  if (!std::isfinite(local) || !std::isfinite(end - start) || end <= start) return;
  const float fraction = std::clamp((local - start) / (end - start), 0.0f, 1.0f);
  const PPCContext saved = ctx;
  ctx.r3.u64 = c;
  ctx.f1.f64 = ReadFloat(c + 152) + fraction * (ReadFloat(c + 156) - ReadFloat(c + 152));
  sub_82B9EF58(ctx, base);
  ctx = saved;
  if (ControlName(c) == "cc2_scrollbar") cc2::display::MouseScrollTo(fraction);
  else if (ControlName(c) == "scrollbar") cc2::display::ScrollText(ctx, base, fraction, false);
}

bool BeginSettingDrag(PPCContext& ctx, uint8_t* base, uint32_t cursor, float x, float y) {
  if (cc2::display::FocusRow()<100) return false;
  for (const uint32_t owner : CursorControls(cursor)) {
    if (!owner || !ControlEnabled(ctx, base, owner)) continue;
    const uint32_t bar=SettingBar(owner);
    if (!bar) continue;
    const int row=SettingBarRow(bar);
    if (!row) continue;
    const auto local=ToLocal(ctx, base, bar, x, y);
    const uint32_t master=Read32(bar+88);
    const float width=ReadFloat(master+64), height=ReadFloat(master+68);
    if (!std::isfinite(local.x) || !std::isfinite(local.y) || width<=0 || height<=0 ||
        std::abs(local.x)>width*0.5f || std::abs(local.y)>height*0.5f) continue;
    // Settings use progress pictures beneath clickable rows, not cursor-registered scrollbar controls.
    g_drag={bar,cursor,Read32(bar+16),Read32(bar+20),0.0f,row,owner};
    DragScroll(ctx, base, x, y);
    return true;
  }
  return false;
}

bool BeginScroll(PPCContext& ctx, uint8_t* base, uint32_t cursor, float x, float y) {
  for (const uint32_t c : CursorControls(cursor)) {
    if (!c || Read32(c) != 0x821C2644 || !ControlEnabled(ctx, base, c)) continue;
    const LocalPoint local = ToLocal(ctx, base, c, x, y);
    const uint32_t master = Read32(c + 88);
    // The shipped mouse area covers half the bar; use the full visible track for mouse presses.
    if (!std::isfinite(local.x) || !std::isfinite(local.y) ||
        std::abs(local.x) > ReadFloat(master + 64) * 0.5f ||
        std::abs(local.y) > ReadFloat(master + 68) * 0.5f) continue;
    const float offset = local.x - ReadFloat(c + 204);
    // Keep the grab point on the thumb; a track click places the thumb at the pointer.
    g_drag = {c, cursor, Read32(c + 16), Read32(c + 20),
              std::abs(offset) <= ReadFloat(c + 188) * 0.5f ? offset : 0.0f};
    DragScroll(ctx, base, x, y);
    return true;
  }
  return false;
}

void SendButton(PPCContext& ctx, uint8_t* base, uint32_t gui, uint32_t button, bool down, float gx, float gy) {
  ctx.r3.u64 = gui;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = Pair(0, kDevice);
  ctx.r6.u64 = Pair(button, 0);
  ctx.r7.u64 = Pair(Bits(gx), Bits(gy));
  ctx.r8.u64 = Pair(1, 0);
  if (down) sub_82AA9EF0(ctx, base);
  else sub_82AA9C30(ctx, base);
}


}  // namespace

void ObservePad(uint32_t user, const rex::input::X_INPUT_GAMEPAD* pad) {
  if (user || !pad) return;
  if (pad->buttons || pad->left_trigger > 30 || pad->right_trigger > 30 ||
      std::abs(int(int16_t(pad->thumb_lx))) > 16000 || std::abs(int(int16_t(pad->thumb_ly))) > 16000 ||
      std::abs(int(int16_t(pad->thumb_rx))) > 16000 || std::abs(int(int16_t(pad->thumb_ry))) > 16000) {
    g_pad_active.store(true);
  }
}

void Initialize(rex::ui::Window* w) {
  g_window.store(w);
  g_mouse_enabled.store(REXCVAR_GET(cc2_mouse));
  w->AddInputListener(&g_listener, 100);
  w->AddListener(&g_listener);
  const HWND hwnd = static_cast<HWND>(w->GetNativeWindowHandle());
  if (!SetWindowSubclass(hwnd, PointerWindowProc, kPointerSubclass, 0))
    REXLOG_ERROR("[mouse] failed to install native cursor window handler: {}", GetLastError());
  else REXLOG_DEBUG("[mouse] native cursor window handler installed");
}

void Shutdown() {
  g_listener.ReleaseCapture();
  if (auto* w = g_window.exchange(nullptr)) {
    RemoveWindowSubclass(static_cast<HWND>(w->GetNativeWindowHandle()), PointerWindowProc, kPointerSubclass);
    w->RemoveInputListener(&g_listener);
    w->RemoveListener(&g_listener);
    w->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    cc2::gpu::SetPointer(0, 0, false);
  }
}

void Tick(PPCContext& ctx, uint8_t* base) {
  auto* w = g_window.load();
  if (!w) return;
  const uint32_t gui = Read32(kGuiInput);
  const uint32_t state = GuiState();
  const uint32_t mask = state ? Read32(state + 92) : 0;
  int32_t mx, my, steps;
  bool moved;
  uint64_t motion_serial;
  std::deque<ButtonEvent> buttons;
  {
    std::lock_guard lock(g_listener.mutex);
    mx = g_listener.x;
    my = g_listener.y;
    moved = g_listener.moved;
    motion_serial = g_listener.motion_serial;
    g_listener.moved = false;
    buttons.swap(g_listener.buttons);
    steps = g_listener.wheel / int32_t(rex::ui::MouseEvent::kScrollPerDetent);
    g_listener.wheel -= steps * int32_t(rex::ui::MouseEvent::kScrollPerDetent);
  }
  float gx = 0, gy = 0, nx = 0, ny = 0;
  const bool inside = ToGui(w, mx, my, gx, gy, nx, ny);
  g_hand_gx = gx;
  g_hand_gy = gy;
  // Windows repeats WM_MOUSEMOVE without motion (cursor show/hide, focus, clicks); a move at the position the GUI
  // already has is dropped so it cannot re-enter the item under the pointer.
  if (moved && mx == g_sent_x && my == g_sent_y) moved = false;
  static bool pending_move = false;
  pending_move = pending_move || moved;
  if (g_pad_active.exchange(false)) {
    g_mouse_active = false;
    pending_move = false;
    cc2::display::MainMenuPointer(ctx,base,-1,false);
  }
  moved = pending_move;
  const bool enabled = REXCVAR_GET(cc2_mouse);
  const bool enabled_changed = g_mouse_enabled.exchange(enabled) != enabled;
  if (cc2::lobby::Active()) {
    // The lobby list owns the pointer: the hand shows over it and no press reaches the GUI underneath.
    auto* screen_pointer = &cc2::lobby::Pointer;
    const PPCContext saved = ctx;
    const bool hand = enabled && g_mouse_active.load();
    const bool menu_changed = !g_menu_cursor.exchange(true, std::memory_order_acq_rel);
    if (enabled_changed || menu_changed || hand != g_hand_published) {
      g_hand_published = hand;
      RefreshPointer(w);
    }
    if (!hand) cc2::gpu::SetPointer(nx, ny, false);
    const bool usable = hand && w->HasFocus();
    if (usable && moved && inside) screen_pointer(ctx, base, gx, gy, -1);
    for (const ButtonEvent& b : buttons) {
      const int i = b.button == kButtonA ? 0 : 1;
      float bx = 0, by = 0, bnx = 0, bny = 0;
      if (b.down) {
        if (!usable || !ToGui(w, b.x, b.y, bx, by, bnx, bny)) continue;
        if (i == 0) g_swallowed = true;
        screen_pointer(ctx, base, bx, by, i);
      } else if (i == 0 && g_swallowed) {
        g_swallowed = false;
      } else if (g_down[i] && gui) {
        g_down[i] = false;
        SendButton(ctx, base, gui, b.button, false, gx, gy);
      }
    }
    pending_move = false;
    ctx = saved;
    return;
  }
  const bool valid = enabled && gui && state && mask && Read8(state + 184) && MenuCursor(state);
  // The pointer itself is the host's, drawn from the UI thread's last mouse event; the guest only publishes
  // whether a menu cursor is live and pulls the sprite when the pad takes over or the menu goes away.
  const bool hand_visible = valid && g_mouse_active;
  const bool menu_changed = g_menu_cursor.exchange(valid, std::memory_order_acq_rel) != valid;
  cc2::display::MouseActive(hand_visible);
  if (!hand_visible) cc2::gpu::SetPointer(nx, ny, false);
  if (enabled_changed || menu_changed || hand_visible != g_hand_published) {
    g_hand_published = hand_visible;
    RefreshPointer(w);
  }
  static bool was_valid = false;
  if (valid && !was_valid) {
    // A new menu screen takes the next move even when it lands where the last screen's pointer was.
    g_sent_x = g_sent_y = INT32_MIN;
  }
  was_valid = valid;
  const PPCContext saved = ctx;
  if (g_drag.control && (!valid || !w->HasFocus() || !g_mouse_active.load() ||
      int32_t(Read32(gui + 4)) > 0 || !DragValid(ctx, base, state))) EndDrag();
  if (!valid) {
    pending_move = false;
    // Releases still go out for presses the GUI saw, so a press that opened a new screen or left the menu is not
    // left held; new presses are dropped.
    if (gui) {
      for (const ButtonEvent& b : buttons) {
        const int i = b.button == kButtonA ? 0 : 1;
        if (b.down) continue;
        if (b.button == kButtonA && g_swallowed) {
          g_swallowed = false;
          continue;
        }
        if (!g_down[i]) continue;
        g_down[i] = false;
        SendButton(ctx, base, gui, b.button, false, gx, gy);
      }
    }
    ctx = saved;
    return;
  }
  if (!(mask & 2)) Write32(state + 92, mask | 2);
  AdoptSettingRows(ctx, base, state);
  if (steps && !g_drag.control && hand_visible && inside && w->HasFocus() && int32_t(Read32(gui + 4)) <= 0)
    cc2::display::MouseWheel(ctx, base, steps);
  if (!moved && buttons.empty()) {
    ctx = saved;
    return;
  }
  static auto last_move = std::chrono::steady_clock::now();
  const int32_t hz = REXCVAR_GET(cc2_mouse_move_hz);
  if (moved && inside && hz > 0 && !g_drag.control) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_move < std::chrono::microseconds(1000000 / hz) && buttons.empty()) moved = false;
    else last_move = now;
  }
  if (!inside || !w->HasFocus() || !g_mouse_active.load()) pending_move = false;
  if (moved && inside && w->HasFocus() && g_mouse_active.load() && int32_t(Read32(gui + 4)) <= 0) {
    // Pad focus requests can replace the next pointer move with a snap; physical mouse motion owns this position.
    *REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(state + 212) = 0;
    ctx.r3.u64 = gui;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = Pair(0, kDevice);            // user 0
    ctx.r6.u64 = 0;
    ctx.r7.u64 = Pair(Bits(gx), Bits(gy));
    ctx.r8.u64 = Pair(1, 0);                  // a peripheral is bound
    sub_82AA9078(ctx, base);
    ctx = saved;
    ctx.r3.u64 = state;
    ctx.r4.u64 = state + 196;
    ctx.r5.u64 = 1;
    sub_82A37E70(ctx, base);
    ctx = saved;
    static uint64_t hover_serial = 0;
    if (motion_serial != hover_serial) {
      hover_serial = motion_serial;
      if (w->HasFocus()) {
        if (!g_drag.control) cc2::display::MouseMoved();
        if (int32_t(Read32(gui + 4)) <= 0 && !g_drag.control) {
          cc2::display::CoursePointer(ctx, base, gx, gy, false);
          const uint32_t target = ClickableAt(ctx, base, state, gx, gy);
          cc2::display::MainMenuPointer(ctx, base, MainMenuParent(target), false);
          cc2::display::ShopPointer(ctx, base, gx, gy, "", target, false);
        }
      }
    }
    pending_move = false;
    g_sent_x = mx;
    g_sent_y = my;
  }
  for (const ButtonEvent& b : buttons) {
    const int i = b.button == kButtonA ? 0 : 1;
    float bx = 0, by = 0, bnx = 0, bny = 0;
    const bool button_inside = ToGui(w, b.x, b.y, bx, by, bnx, bny);
    if (b.down) {
      // Presses count only inside the game image. A release that never reached the GUI is sent before the next
      // press, so a lost release costs no click.
      if (!button_inside || !w->HasFocus()) continue;
      if (i == 0) { g_swallowed = false; EndDrag(); }
      if (g_down[i]) SendButton(ctx, base, gui, b.button, false, gx, gy);
      g_down[i] = false;
      if (i == 0 && int32_t(Read32(gui + 4)) <= 0 && cc2::display::CoursePointer(ctx, base, bx, by, true)) {
        g_swallowed = true;
        continue;
      }
      if (i == 0 && int32_t(Read32(gui + 4)) <= 0) {
        const uint32_t target = ClickableAt(ctx, base, state, bx, by);
        const uint32_t parent = target ? Read32(target + 76) : 0;
        const std::string_view tab = parent && ControlName(parent) == "options" ? ControlName(target) : "";
        if (cc2::display::ShopPointer(ctx, base, bx, by, tab, target, true)) {
          g_swallowed = true;
          continue;
        }
      }
      if (i == 0 && int32_t(Read32(gui + 4)) <= 0 &&
          (BeginSettingDrag(ctx, base, state, bx, by) || BeginScroll(ctx, base, state, bx, by))) {
        g_swallowed = true;
        continue;
      }
      // A left click on one of our settings rows steps that row instead of reaching the GUI as A; the matching
      // release is swallowed with it.
      if (i == 0 && cc2::display::MouseClick(SettingAt(ctx, base, state, bx, by))) {
        g_swallowed = true;
        continue;
      }
      if (i == 0) {
        const uint32_t target = ClickableAt(ctx, base, state, bx, by);
        g_swallowed = true;
        if (target && int32_t(Read32(gui + 4)) <= 0) {
          const int parent=MainMenuParent(target);
          if (!cc2::display::MainMenuPointer(ctx,base,parent,true))
            ClickControl(ctx, base, gui, state, target, bx, by);
        }
        continue;
      }
      g_down[i] = true;
    } else {
      // Releases pair with the press the GUI saw, wherever the pointer is now.
      if (i == 0 && g_swallowed) {
        if (g_drag.control) {
          DragScroll(ctx, base, bx, by);
          EndDrag();
        }
        g_swallowed = false;
        continue;
      }
      if (!g_down[i]) continue;
      g_down[i] = false;
    }
    SendButton(ctx, base, gui, b.button, b.down, bx, by);
  }
  if (g_drag.control && moved) DragScroll(ctx, base, gx, gy);
  ctx = saved;
}

// With the mouse, main-menu child buttons and shop tabs and items hit-test against their visible art; the hit
// rectangles their masters define are sized for controller focus.
REX_HOOK_RAW(sub_82A22F60) {
  const uint32_t control=ctx.r3.u32;
  if (g_mouse_active.load()) {
    const uint32_t definition=Read32(control+88);
    const auto master=ControlName(definition);
    const auto name=ControlName(control);
    if (MainMenuControl(control) && (name=="sub_1" || name=="sub_2" || name=="sub_3" || name=="sub_4") &&
        master!="common/ui/men/opt/empty") {
      const float width=ReadFloat(definition+64), height=ReadFloat(definition+68);
      if (width>0 && height>0) {
        const auto p=ToLocal(ctx,base,control,float(ctx.f1.f64),float(ctx.f2.f64));
        // The child buttons ship with a hit region only 30% of their visible height.
        const float half_height=std::max(1.0f,ReadFloat(definition+188))*0.5f;
        ctx.r3.u64=std::abs(p.x/width-ReadFloat(definition+176))<=ReadFloat(definition+184)*0.5f &&
                   std::abs(p.y/height-ReadFloat(definition+180))<=half_height;
        return;
      }
    }
    const bool tab=master=="common/ui/men/opt/shop/featured" || master=="common/ui/men/opt/shop/rewinds" ||
                   master=="common/ui/men/opt/shop/powerups" || master=="common/ui/men/opt/shop/jinxes" ||
                   master=="common/ui/men/opt/shop/avatar_fx" || master=="common/ui/men/opt/shop/courses";
    const bool item=master=="common/ui/men/shop/item";
    if (tab || item) {
      const auto p=ToLocal(ctx,base,control,float(ctx.f1.f64),float(ctx.f2.f64));
      ctx.r3.u64=std::abs(p.x)<=(tab ? 0.0915f : 0.215f) && std::abs(p.y)<=(tab ? 0.035f : 0.215f);
      return;
    }
  }
  __imp__sub_82A22F60(ctx,base);
}

// Main-menu parent hover is visual only for the mouse; the shipped handlers replace submenu controls.
REX_HOOK_RAW(sub_82A22D38) {
  if (g_mouse_active.load() && MainMenuParent(ctx.r3.u32)) return;
  __imp__sub_82A22D38(ctx, base);
}
REX_HOOK_RAW(sub_82A22E38) {
  if (g_mouse_active.load() && MainMenuParent(ctx.r3.u32)) return;
  __imp__sub_82A22E38(ctx, base);
}

// A controller's default selection must not move the mouse to another main-menu button.
REX_HOOK_RAW(sub_82AA94A0) {
  if (g_mouse_active.load() && MainMenuControl(ctx.r3.u32)) return;
  __imp__sub_82AA94A0(ctx, base);
}


namespace {
// The pointer the game's tutorials place next to a control: the hand_pointer model, lit and post-processed with the
// rest of the GUI. It sits on the first listed layer that exists, at the highest z-sort the GUI still draws (255), so
// it covers every panel and popup; its absolute position corrects its relative one every frame.
constexpr char kGloveScript[] = R"lua(
function CC2GloveTick(show,x,y,size,layers,angle,zsort)
  if not gui then return -3 end
  local layer=nil
  local index=0
  -- An entry "layer:control" only counts while that control is up, which is how the pause menu's layer wins in a level.
  for entry in string.gmatch(layers,"[^;]+") do
    local name,need=string.match(entry,"^([^:]+):?(.*)$")
    index=index+1
    local l=gui[name]
    if l and (need=="" or (l[need] and type(l[need].GetAbsScreenPos)=="function")) then layer=name break end
  end
  if not layer then return -4 end
  if not show then
    -- Hidden on every listed layer, so a glove left on a layer that stopped being first cannot linger.
    for name in string.gmatch(layers,"[^;:]+:?[^;]*") do
      name=string.match(name,"^[^:]+")
      local g=gui[name] and gui[name].glove
      if g and type(g.GetAbsScreenPos)=="function" then g.Visible=false end
    end
  end
  if CC2GloveLayer~=layer then
    local old=CC2GloveLayer and gui[CC2GloveLayer]
    if old and old.glove and type(old.glove.GetAbsScreenPos)=="function" then old.glove.Visible=false end
    CC2GloveLayer=layer
    CC2GloveWait=0
  end
  local p=gui[layer]
  local c=p.glove
  if not (c and type(c.GetAbsScreenPos)=="function") then
    -- Boot screens and levels never get a glove: it is only created once a menu cursor wants it.
    if not show then return 5 end
    -- A screen change replaces the layer's controls, so a missing glove is added again after a short wait.
    CC2GloveWait=(CC2GloveWait or 0)-1
    if CC2GloveWait<=0 and type(p.AddControl)=="function" then
      p.AddControl("glove","GRES:common/oa/pointer0",CreateV2(0,0),8)
      CC2GloveWait=30
    end
    return index*10+4
  end
  c.Visible=show
  c.Enabled=false
  if not show then return index*10+1 end
  if size>0 then c.RelSize=CreateV2(size,size) end
  c.RelOrientation=c.RelOrientation+(angle-c.GetAbsOrientation())
  c.RelZSort=c.RelZSort+(zsort-c.GetAbsZSort())
  local a=c.GetAbsScreenPos()
  local r=c.RelScreenPos
  c.RelScreenPos=CreateV2(r.x+(x-a.x),r.y+(y-a.y))
  return index*10+1
end
return 1
)lua";
uint32_t g_glove_state = 0, g_glove_thread = 0;
}  // namespace

void GloveTick(PPCContext& ctx, uint8_t* base) {
  const bool wanted = REXCVAR_GET(cc2_native_glove);
  static bool live = false;
  auto set_live = [&](bool value) {
    if (live == value) return;
    live = value;
    cc2::gpu::SuppressPointer(value);
    REXLOG_DEBUG("[mouse] native glove {}", value ? "drawing the hand" : "off; the baked sprite draws the hand");
  };
  const PPCContext saved = ctx;
  const uint32_t state = cc2::lobby::MainLuaState();
  if (!state) {
    set_live(wanted);
    return;
  }
  if (state != g_glove_state) {
    // Per-frame calls run on a coroutine of the main state, the way the lobby list does, not on the main stack.
    g_glove_thread = 0;
    if (cc2::lobby::RunLua(ctx, base, state, "CC2GloveThread=coroutine.create(function() end); return CC2GloveThread",
                           &g_glove_thread) < 0 || !g_glove_thread ||
        cc2::lobby::RunLua(ctx, base, g_glove_thread, kGloveScript) != 1) {
      ctx = saved;
      set_live(wanted);
      return;
    }
    g_glove_state = state;
  }
  const bool show = wanted && g_hand_published;
  // The model's fingertip sits tip units from its pivot along the pointing direction; GUI x units are 4/3 as wide.
  const double angle = REXCVAR_GET(cc2_glove_angle);
  const double radians = angle * 3.14159265358979 / 180.0;
  const double tip = REXCVAR_GET(cc2_glove_tip);
  const float x = g_hand_gx + float(REXCVAR_GET(cc2_glove_offset_x) - std::cos(radians) * tip * 0.75);
  const float y = g_hand_gy + float(REXCVAR_GET(cc2_glove_offset_y) - std::sin(radians) * tip);
  // The lobby list sits above screens, so the glove moves to popups while it's open.
  const std::string layers = (cc2::lobby::Active() ? std::string("popups;") : std::string()) +
                             REXCVAR_GET(cc2_glove_layer);
  char call[320];
  std::snprintf(call, sizeof(call), "return CC2GloveTick(%s,%.6f,%.6f,%.6f,\"%s\",%.3f,%d)", show ? "true" : "false",
                x, y, float(REXCVAR_GET(cc2_glove_size)), layers.c_str(), angle,
                int(REXCVAR_GET(cc2_glove_zsort)));
  const int result = cc2::lobby::RunLua(ctx, base, g_glove_thread, call);
  ctx = saved;
  static int last_result = 0;
  if (result != last_result) {
    REXLOG_DEBUG("[mouse] native glove layer {} stage {}", result / 10, result % 10);
    last_result = result;
  }
  // With the native glove on, the baked sprite never draws; the mouse keeps only its behaviour.
  set_live(wanted);
}

}  // namespace cc2::mouse
