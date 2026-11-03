#include "cc2/freecam.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>
#include <rex/ui/window.h>
#include <windows.h>

REXCVAR_DEFINE_DOUBLE(cc2_freecam_speed, 8.0, "Input", "Free camera speed in world units per second; Shift or RB is 4x");
REXCVAR_DEFINE_DOUBLE(cc2_freecam_mouse, 0.12, "Input", "Free camera mouse look in degrees per pixel");
REXCVAR_DEFINE_DOUBLE(cc2_freecam_stick, 120.0, "Input", "Free camera right-stick look in degrees per second");
REXCVAR_DEFINE_DOUBLE(cc2_close_up_distance, -1.5225, "Display", "Close-up camera distance behind the player");
REXCVAR_DEFINE_DOUBLE(cc2_close_up_side, 5.865, "Display", "Close-up camera offset to the side of the player");
REXCVAR_DEFINE_DOUBLE(cc2_close_up_height, 3.24, "Display", "Close-up camera height above the player");
REXCVAR_DEFINE_DOUBLE(cc2_close_up_ahead, 2.5, "Display", "How far ahead of the player the close-up camera aims");
REXCVAR_DEFINE_DOUBLE(cc2_close_up_aim_height, 1.4, "Display", "Height above the player the close-up camera aims at");
REXCVAR_DEFINE_DOUBLE(cc2_close_up_cull_scale, 4.0, "Display",
                      "Close-up camera: an object is culled only when its bounds scaled by this are out of view too");

extern "C" REX_FUNC(__imp__sub_82AB9990);  // CAMERA::UpdateMatrices
extern "C" REX_FUNC(__imp__sub_82ABA328);  // CAMERA::Update (vtable +432)
extern "C" REX_FUNC(__imp__sub_82B3D2D0);  // SHOOTERCAMERA::Update, the gameplay camera's own follow logic
extern "C" REX_FUNC(__imp__sub_82BDD568);  // Frustum_TestBounds: 0 inside, 1 crossing, 2 outside

namespace cc2::freecam {
namespace {
constexpr uint32_t kGameXA = 0x82EAA328;
constexpr uint32_t kScene = 0x82EDBB1C;
constexpr uint32_t kActorList = 54044;  // scene + this: 16-byte nodes, actor pointer at +4

uint32_t Read32(uint32_t address) { return *REX_KERNEL_MEMORY()->TranslateVirtual<const rex::be<uint32_t>*>(address); }
float ReadFloat(uint32_t address) { return *REX_KERNEL_MEMORY()->TranslateVirtual<const rex::be<float>*>(address); }
void WriteFloat(uint32_t address, float value) { *REX_KERNEL_MEMORY()->TranslateVirtual<rex::be<float>*>(address) = value; }
void Write8(uint32_t address, uint8_t value) { *REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address) = value; }

// Scene actor lookup the way the game does it: the handle's high half indexes the node table and the actor's own
// handle at +20 must match.
uint32_t Actor(uint32_t list, uint32_t handle) {
  const int16_t index = int16_t(handle >> 16);
  if (!list || index < 0 || index >= 6000) return 0;
  const uint32_t actor = Read32(list + 16 * uint32_t(index) + 4);
  return actor && Read32(actor + 20) == handle ? actor : 0;
}

// Player 0's camera actor: GAMEXA keeps a 64-byte slot per player holding the player's 64-bit actor handle, and the
// player actor holds its camera's handle at +1664; both resolve through the scene's actor table by the low half.
uint32_t PlayerActor() {
  const uint32_t game = Read32(kGameXA), scene = Read32(kScene);
  if (!game || !scene || int32_t(Read32(game + 152)) <= 0) return 0;
  const uint32_t handle = Read32(game + 160 + 4);
  if (!(handle & 0xFFFF)) return 0;
  return Actor(scene + kActorList, handle);
}
uint32_t GameplayCamera() {
  const uint32_t player = PlayerActor();
  return player ? Actor(Read32(kScene) + kActorList, Read32(player + 1668)) : 0;
}

struct Vec { float x, y, z; };
Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Scale(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec Add(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
// The game camera's up vector banks with the track; the world's vertical is the axis it leans along.
Vec WorldUp(Vec up) {
  const float ax = std::abs(up.x), ay = std::abs(up.y), az = std::abs(up.z);
  if (ay >= ax && ay >= az) return {0, up.y < 0 ? -1.0f : 1.0f, 0};
  if (az >= ax) return {0, 0, up.z < 0 ? -1.0f : 1.0f};
  return {up.x < 0 ? -1.0f : 1.0f, 0, 0};
}
Vec Normal(Vec a) {
  const float length = std::sqrt(Dot(a, a));
  return length > 1e-6f ? Scale(a, 1.0f / length) : Vec{0, 0, 1};
}

std::atomic<rex::ui::Window*> g_window{nullptr};
std::atomic<bool> g_active{false}, g_paused{false}, g_close_up{false};
std::atomic<uint32_t> g_camera{0};
std::mutex g_pad_mutex;
rex::input::X_INPUT_GAMEPAD g_pad{};
// Camera pose in a basis built from the game's up vector: forward = cos(pitch) * (cos(yaw) * ahead + sin(yaw) *
// right) + sin(pitch) * up. The pose is published for the UpdateMatrices hook under the mutex.
std::mutex g_pose_mutex;
Vec g_pos{}, g_up{0, 1, 0}, g_ahead{0, 0, 1}, g_right{1, 0, 0};
float g_yaw = 0, g_pitch = 0;
bool g_boost = false;
// Close-up follow: the camera trails the player along the direction of travel, which is taken from the player's
// own motion because the actor's facing is not read here.
Vec g_cu_pos{}, g_cu_look{0, 0, 1}, g_cu_facing{1, 0, 0}, g_cu_prev{}, g_cu_aim{}, g_cu_up{0, 1, 0};
float g_cu_side = 1;  // which side of the track the camera sits on, taken from the game's own camera
bool g_cu_valid = false;

Vec Forward() {
  const float cp = std::cos(g_pitch);
  return Add(Add(Scale(g_ahead, cp * std::cos(g_yaw)), Scale(g_right, cp * std::sin(g_yaw))), Scale(g_up, std::sin(g_pitch)));
}

// Takes the pose over from the game's camera so the view does not jump when the free camera starts.
void Capture(uint32_t camera) {
  g_pos = {ReadFloat(camera + 108), ReadFloat(camera + 112), ReadFloat(camera + 116)};
  g_up = WorldUp({ReadFloat(camera + 1444), ReadFloat(camera + 1448), ReadFloat(camera + 1452)});
  const Vec look = Normal({ReadFloat(camera + 1456), ReadFloat(camera + 1460), ReadFloat(camera + 1464)});
  const float rise = std::clamp(Dot(look, g_up), -0.999f, 0.999f);
  Vec flat = Normal(Add(look, Scale(g_up, -rise)));
  if (std::abs(Dot(flat, flat)) < 1e-6f) flat = Normal(Cross(g_up, Vec{1, 0, 0}));
  g_ahead = flat;
  g_right = Normal(Cross(g_up, g_ahead));
  g_yaw = 0;
  g_pitch = std::asin(rise);
}

bool KeyDown(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

void Fly(float dt) {
  auto* w = g_window.load();
  const bool focused = w && w->HasFocus();
  HWND hwnd = w ? static_cast<HWND>(w->GetNativeWindowHandle()) : nullptr;
  float look_yaw = 0, look_pitch = 0;
  Vec move{0, 0, 0};
  bool boost = false;
  if (focused && hwnd) {
    // Mouse look from the distance to the window centre, with the cursor put back there every frame.
    RECT client;
    POINT cursor;
    if (GetClientRect(hwnd, &client) && GetCursorPos(&cursor)) {
      POINT centre{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
      ClientToScreen(hwnd, &centre);
      static bool warped = false;
      if (warped) {
        const float degrees = float(REXCVAR_GET(cc2_freecam_mouse));
        look_yaw += float(cursor.x - centre.x) * degrees;
        look_pitch -= float(cursor.y - centre.y) * degrees;
      }
      SetCursorPos(centre.x, centre.y);
      warped = true;
    }
    if (KeyDown('W')) move.z += 1;
    if (KeyDown('S')) move.z -= 1;
    if (KeyDown('D')) move.x += 1;
    if (KeyDown('A')) move.x -= 1;
    if (KeyDown('E') || KeyDown(VK_SPACE)) move.y += 1;
    if (KeyDown('Q') || KeyDown(VK_CONTROL)) move.y -= 1;
    boost = KeyDown(VK_SHIFT);
  }
  {
    std::lock_guard lock(g_pad_mutex);
    auto axis = [](int16_t v) { const float f = v / 32767.0f; return std::abs(f) > 0.2f ? f : 0.0f; };
    move.x += axis(int16_t(g_pad.thumb_lx));
    move.z += axis(int16_t(g_pad.thumb_ly));
    move.y += (g_pad.right_trigger - g_pad.left_trigger) / 255.0f;
    const float stick = float(REXCVAR_GET(cc2_freecam_stick)) * dt;
    look_yaw += axis(int16_t(g_pad.thumb_rx)) * stick;
    look_pitch += axis(int16_t(g_pad.thumb_ry)) * stick;
    boost = boost || (g_pad.buttons & 0x0200);
  }
  constexpr float kDegrees = 3.14159265f / 180.0f;
  g_yaw += look_yaw * kDegrees;
  g_pitch = std::clamp(g_pitch + look_pitch * kDegrees, -1.55f, 1.55f);
  const float speed = float(REXCVAR_GET(cc2_freecam_speed)) * (boost ? 4.0f : 1.0f) * dt;
  const Vec forward = Forward();
  const Vec right = Normal(Cross(g_up, forward));
  g_pos = Add(g_pos, Add(Add(Scale(forward, move.z * speed), Scale(right, move.x * speed)), Scale(g_up, move.y * speed)));
}

// Until the player moves, run along the track the way the game camera looks across it: the travel direction is
// perpendicular to that view, and the game camera's side of the track becomes ours.
void TrackFrame(uint32_t camera, Vec p) {
  g_cu_up = WorldUp({ReadFloat(camera + 1444), ReadFloat(camera + 1448), ReadFloat(camera + 1452)});
  const Vec cam{ReadFloat(camera + 108), ReadFloat(camera + 112), ReadFloat(camera + 116)};
  const Vec look{ReadFloat(camera + 1456), ReadFloat(camera + 1460), ReadFloat(camera + 1464)};
  Vec across = Add(look, Scale(g_cu_up, -Dot(look, g_cu_up)));
  across = Dot(across, across) > 1e-6f ? Normal(across) : Vec{0, 0, 1};
  g_cu_facing = Normal(Cross(g_cu_up, across));
  g_cu_side = Dot(Add(cam, Scale(p, -1.0f)), Cross(g_cu_up, g_cu_facing)) < 0 ? -1.0f : 1.0f;
}

// Logs the free camera's pose as close-up cvar values, in the track frame the free camera started in.
void LogClosePose() {
  const uint32_t player = PlayerActor();
  if (!player) return;
  const Vec p{ReadFloat(player + 108), ReadFloat(player + 112), ReadFloat(player + 116)};
  const Vec up = g_cu_up, side = Scale(Cross(up, g_cu_facing), g_cu_side);
  const Vec gap = Add(g_pos, Scale(p, -1.0f));
  const Vec look = Forward();
  // The aim point is where the view crosses the vertical plane along the track through the player.
  const float across = Dot(look, side);
  const float t = std::abs(across) > 1e-4f ? -Dot(gap, side) / across : 0.0f;
  const Vec hit = Add(gap, Scale(look, std::max(t, 0.0f)));
  const float values[] = {-Dot(gap, g_cu_facing), Dot(gap, side), Dot(gap, up), Dot(hit, g_cu_facing), Dot(hit, up)};
  static float logged[5] = {};
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  bool moved = false;
  for (int i = 0; i < 5; ++i) moved = moved || std::abs(values[i] - logged[i]) > 0.05f;
  if (!moved || now - last < std::chrono::milliseconds(500)) return;
  std::copy(std::begin(values), std::end(values), logged);
  last = now;
  REXLOG_INFO("[freecam] close-up pose: distance {:.2f} side {:.2f} height {:.2f} ahead {:.2f} aim_height {:.2f}",
              values[0], values[1], values[2], values[3], values[4]);
}
}  // namespace

// Moves the follow camera one frame along; dt 0 holds the pose (the pause menu). The framing is a three-quarter
// view: behind the player, out to the side the game's camera uses, and above, aimed a little ahead of them.
void Follow(float dt, uint32_t camera) {
  const uint32_t player = PlayerActor();
  if (!player) return;
  const Vec p{ReadFloat(player + 108), ReadFloat(player + 112), ReadFloat(player + 116)};
  if (!g_cu_valid) {
    TrackFrame(camera, p);
    g_cu_prev = p;
    g_cu_aim = p;
    g_cu_pos = {ReadFloat(camera + 108), ReadFloat(camera + 112), ReadFloat(camera + 116)};
    g_cu_valid = true;
  }
  const Vec up = g_cu_up;
  if (dt > 0) {
    const Vec velocity = Scale(Add(p, Scale(g_cu_prev, -1.0f)), 1.0f / dt);
    g_cu_prev = p;
    const Vec flat = Add(velocity, Scale(up, -Dot(velocity, up)));
    // Only real travel turns the camera, and slowly, so steps and stumbles do not swing the view.
    if (Dot(flat, flat) > 4.0f) {
      const Vec target = Normal(flat);
      g_cu_facing = Normal(Add(g_cu_facing, Scale(Add(target, Scale(g_cu_facing, -1.0f)), std::min(1.0f, dt * 2.0f))));
    }
    const Vec side = Scale(Cross(up, g_cu_facing), g_cu_side);
    const float distance = float(REXCVAR_GET(cc2_close_up_distance));
    const float lateral = float(REXCVAR_GET(cc2_close_up_side));
    const float height = float(REXCVAR_GET(cc2_close_up_height));
    // The aim point follows the player along the track quickly but their height slowly, so a jump moves the
    // avatar in the frame rather than the frame with the avatar.
    const Vec gap = Add(p, Scale(g_cu_aim, -1.0f));
    const float rise = Dot(gap, up);
    const Vec level = Add(gap, Scale(up, -rise));
    g_cu_aim = Add(g_cu_aim, Add(Scale(level, std::min(1.0f, dt * 8.0f)), Scale(up, rise * std::min(1.0f, dt * 1.5f))));
    const Vec target = Add(Add(Add(g_cu_aim, Scale(g_cu_facing, -distance)), Scale(side, lateral)), Scale(up, height));
    g_cu_pos = Add(g_cu_pos, Scale(Add(target, Scale(g_cu_pos, -1.0f)), std::min(1.0f, dt * 4.0f)));
  }
  const float ahead = float(REXCVAR_GET(cc2_close_up_ahead));
  const Vec aim = Add(Add(g_cu_aim, Scale(g_cu_facing, ahead)), Scale(up, float(REXCVAR_GET(cc2_close_up_aim_height))));
  g_cu_look = Normal(Add(aim, Scale(g_cu_pos, -1.0f)));
}

// Writes the pose we own into the camera actor: position, look offset and a fixed up vector, with the view marked
// dirty so the next UpdateMatrices rebuilds it. Called from the camera's update and again before the matrices.
void ApplyPoseImpl(uint32_t camera) {
  const bool free = g_active.load(), close_up = g_close_up.load();
  if (!(free || close_up) || camera != g_camera.load() || Read32(camera + 1392) != 0) return;
  std::lock_guard lock(g_pose_mutex);
  if (!free && !g_cu_valid) return;
  const Vec pos = free ? g_pos : g_cu_pos;
  const Vec look = free ? Forward() : g_cu_look;
  const Vec up = free ? g_up : g_cu_up;
  WriteFloat(camera + 108, pos.x);
  WriteFloat(camera + 112, pos.y);
  WriteFloat(camera + 116, pos.z);
  WriteFloat(camera + 1444, up.x);
  WriteFloat(camera + 1448, up.y);
  WriteFloat(camera + 1452, up.z);
  WriteFloat(camera + 1456, look.x);
  WriteFloat(camera + 1460, look.y);
  WriteFloat(camera + 1464, look.z);
  Write8(camera + 1516, 1);
}

void ApplyPose(uint32_t camera) { ::cc2::freecam::ApplyPoseImpl(camera); }
void Initialize(rex::ui::Window* window) { g_window.store(window); }
void Shutdown() {
  g_window.store(nullptr);
  g_active.store(false);
}
bool Active() { return g_active.load(); }
uint32_t OverriddenFrustum() { const uint32_t camera = g_camera.load(); return camera ? camera + 1780 : 0; }

void Tick(bool enabled, bool close_up, bool course, bool paused) {
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
  last = now;
  const uint32_t camera = (enabled || close_up) && course ? GameplayCamera() : 0;
  const bool was = g_active.load();
  if (!camera) {
    if (was) REXLOG_INFO("[freecam] off");
    if (g_close_up.load()) REXLOG_INFO("[freecam] close up off");
    g_active.store(false);
    g_close_up.store(false);
    g_cu_valid = false;
    g_camera.store(0);
    return;
  }
  std::lock_guard lock(g_pose_mutex);
  if (!enabled) {
    // The free camera has priority; the follow camera runs when only its row is on.
    if (was) REXLOG_INFO("[freecam] off");
    g_active.store(false);
    if (!g_close_up.load() || g_camera.load() != camera) {
      g_cu_valid = false;
      g_camera.store(camera);
      g_close_up.store(true);
      REXLOG_INFO("[freecam] close up on");
    }
    g_paused.store(paused);
    Follow(paused ? 0.0f : dt, camera);
    return;
  }
  // The pose readout keeps the close-up camera's track frame when the free camera takes over from it.
  const bool framed = g_close_up.load() && g_cu_valid;
  if (g_close_up.load()) {
    g_close_up.store(false);
    g_cu_valid = false;
  }
  if (!was || g_camera.load() != camera) {
    if (const uint32_t player = PlayerActor(); player && !framed) {
      TrackFrame(camera, {ReadFloat(player + 108), ReadFloat(player + 112), ReadFloat(player + 116)});
    }
    Capture(camera);
    g_camera.store(camera);
    g_active.store(true);
    REXLOG_INFO("[freecam] on at ({:.1f}, {:.1f}, {:.1f})", g_pos.x, g_pos.y, g_pos.z);
  }
  // The pause menu takes the sticks and the mouse back; the camera holds its pose until the course resumes.
  g_paused.store(paused);
  if (!paused) {
    Fly(dt);
    LogClosePose();
  }
}

void FilterInput(uint32_t user, rex::input::X_INPUT_GAMEPAD* pad) {
  if (user || !pad || !g_active.load() || g_paused.load()) return;
  std::lock_guard lock(g_pad_mutex);
  g_pad = *pad;
  // The course sees only Start and Back, so the player stands still while the camera flies and pausing still works.
  pad->buttons = uint16_t(pad->buttons) & uint16_t(0x0030);
  pad->left_trigger = pad->right_trigger = 0;
  pad->thumb_lx = pad->thumb_ly = pad->thumb_rx = pad->thumb_ry = 0;
}
}  // namespace cc2::freecam

// The game's follow logic writes the camera at the end of its update; ours goes in right after, so culling and
// everything else that reads the camera this frame sees the same pose the view matrix is built from.
REX_HOOK_RAW(sub_82B3D2D0) {
  const uint32_t camera = ctx.r3.u32;
  __imp__sub_82B3D2D0(ctx, base);
  cc2::freecam::ApplyPose(camera);
}
REX_HOOK_RAW(sub_82ABA328) {
  const uint32_t camera = ctx.r3.u32;
  __imp__sub_82ABA328(ctx, base);
  cc2::freecam::ApplyPose(camera);
}
REX_HOOK_RAW(sub_82AB9990) {
  cc2::freecam::ApplyPose(ctx.r3.u32);
  __imp__sub_82AB9990(ctx, base);
}


// Objects are culled by bounds that only match the designed side view; seen from elsewhere they vanish at the frustum
// edges and far plane while still on screen. Against the gameplay camera's own frustum (camera + 1780), the free
// camera passes every test; close up, an object the game calls outside is tested again with its bounds scaled by
// cc2_close_up_cull_scale. The shadow and other passes keep the game's culling.
REX_HOOK_RAW(sub_82BDD568) {
  const uint32_t frustum = ctx.r3.u32, bounds = ctx.r4.u32;
  if (!frustum || frustum != cc2::freecam::OverriddenFrustum()) return __imp__sub_82BDD568(ctx, base);
  if (cc2::freecam::Active()) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82BDD568(ctx, base);
  const float scale = float(REXCVAR_GET(cc2_close_up_cull_scale));
  if (ctx.r3.u32 != 2 || !bounds || scale <= 1.0f) return;
  // Bounds: AABB min +0 and max +12, valid flag +49, eight corners from +52; the copy sits in a frame of our own
  // above the stack pointer the retest runs with.
  constexpr uint32_t kBoundsSize = 148;
  auto* memory = REX_KERNEL_MEMORY();
  const uint32_t sp = ctx.r1.u32;
  const uint32_t frame = (sp - 256) & ~15u;
  const uint32_t copy = frame + 64;
  std::memcpy(memory->TranslateVirtual<uint8_t*>(copy), memory->TranslateVirtual<const uint8_t*>(bounds), kBoundsSize);
  auto* f = memory->TranslateVirtual<rex::be<float>*>(copy);
  float centre[3];
  for (int a = 0; a < 3; ++a) centre[a] = (float(f[a]) + float(f[3 + a])) * 0.5f;
  for (int a = 0; a < 3; ++a) {
    f[a] = centre[a] + (float(f[a]) - centre[a]) * scale;
    f[3 + a] = centre[a] + (float(f[3 + a]) - centre[a]) * scale;
  }
  for (int corner = 0; corner < 8; ++corner) {
    for (int a = 0; a < 3; ++a) {
      auto& v = f[13 + corner * 3 + a];
      v = centre[a] + (float(v) - centre[a]) * scale;
    }
  }
  ctx.r1.u32 = frame;
  ctx.r3.u64 = frustum;
  ctx.r4.u64 = copy;
  __imp__sub_82BDD568(ctx, base);
  ctx.r1.u32 = sp;
}
