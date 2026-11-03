#pragma once

namespace rex::input {
class InputSystem;
}

namespace cc2::kernel::xam {
// Controllers by connection order, the keyboard and mouse with player 1 (player 2 under player_2_mnk) except while
// the Guide is up.
void InstallInputAssignment(rex::input::InputSystem* input);
}  // namespace cc2::kernel::xam
