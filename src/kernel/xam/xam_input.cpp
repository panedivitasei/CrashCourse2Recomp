// XamInputGetState, title-side.
//
// Overrides the runtime's export (the exe's __imp__ definition wins over the
// DLL import) for a defined-state guarantee on the output buffer and the
// Guide's pad reads (NoteGamePad).

#pragma GCC diagnostic ignored "-Wunused-parameter"

#include "cc2/kernel/xam/input_assignment.h"
#include "cc2/display_settings.h"
#include "cc2/freecam.h"
#include "cc2/mouse_ui.h"
#include "cc2/lobby.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>

#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>
#include <rexglue_guide/guide.h>

namespace rex::kernel::xam {

using namespace rex;
using namespace rex::system;

namespace {

constexpr uint32_t XINPUT_FLAG_GAMEPAD = 0x01;
constexpr uint32_t XINPUT_FLAG_ANY_USER = 1 << 30;

rex::input::InputSystem* input_system() {
  return static_cast<rex::input::InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
}

}  // namespace

u32 XamInputGetState_entry(u32 user_index, u32 flags,
                           ppc_ptr_t<rex::input::X_INPUT_STATE> input_state) {
  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore queries for other device types. Zero the output: real XAM leaves
    // it defined and games rely on that. CC1's pad poller ORs the big-button
    // state in without checking the return code, so stale stack data would
    // read as held buttons.
    input_state.Zero();
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    actual_user_index = 0;  // always pin user to 0
  }

  auto result = input_system()->GetState(actual_user_index, input_state);
  if (result == X_ERROR_DEVICE_NOT_CONNECTED) {
    input_state.Zero();  // same defined-state guarantee as above
  }

  if (result == X_ERROR_SUCCESS && input_state) {
    // Unfiltered read for the Guide.
    const auto& read = input_state->gamepad;
    rexglue_guide::NoteGamePad(int(actual_user_index), read.buttons, read.left_trigger, read.right_trigger,
                               read.thumb_lx, read.thumb_ly);
    // The game sees nothing while the Guide is up; the Guide still gets each player's pad above.
    if (rexglue_guide::IsOpen()) input_state->gamepad = rex::input::X_INPUT_GAMEPAD{};
    cc2::mouse::ObservePad(actual_user_index, &input_state->gamepad);
    cc2::display::FilterInput(actual_user_index, &input_state->gamepad);
    cc2::freecam::FilterInput(actual_user_index, &input_state->gamepad);
    cc2::lobby::FilterInput(actual_user_index, &input_state->gamepad);
  }
  return result;
}

}  // namespace rex::kernel::xam

REX_EXPORT(__imp__XamInputGetState, rex::kernel::xam::XamInputGetState_entry)

REXCVAR_DEFINE_BOOL(player_2_mnk, false, "Input",
                    "Keyboard and mouse play player 2, and player 1 takes the first controller; read at startup");

namespace cc2::kernel::xam {
namespace {

// The SDK's slot assignment, with the keyboard and mouse on player 2 under player_2_mnk. While the Guide is up the
// keyboard and mouse feed no player: the Guide reads them itself, and each player's controller still reaches it.
class TitleAssignment final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override {
    for (auto& user : users_) user.clear();
    keyboard_ = rex::input::DeviceId::kInvalid;
    for (const auto& device : devices) {
      const bool keyboard = device.synthetic && device.name == "Keyboard and Mouse";
      if (keyboard) keyboard_ = device.id;
      if (device.synthetic) {
        users_[keyboard && REXCVAR_GET(player_2_mnk) ? 1 : 0].push_back(device.id);
      } else if (device.ordinal < rex::input::kMaxGuestUsers) {
        users_[device.ordinal].push_back(device.id);
      }
    }
  }
  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override {
    out.clear();
    if (user_index >= rex::input::kMaxGuestUsers) return;
    out = users_[user_index];
    if (rexglue_guide::IsOpen()) std::erase(out, keyboard_);
  }

 private:
  std::array<std::vector<rex::input::DeviceId>, rex::input::kMaxGuestUsers> users_;
  rex::input::DeviceId keyboard_ = rex::input::DeviceId::kInvalid;
};

}  // namespace

void InstallInputAssignment(rex::input::InputSystem* input) {
  if (!input) return;
  input->SetDeviceAssignment(std::make_unique<TitleAssignment>());
  if (REXCVAR_GET(player_2_mnk)) REXKRNL_INFO("[input] keyboard and mouse play player 2");
}

}  // namespace cc2::kernel::xam
