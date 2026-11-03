#pragma once
#include <cstdint>
#include <rex/ppc/context.h>

namespace cc2::fame {
// Places a local lobby slot's FameStar counter once the slot has slid in; run every frame on the game thread.
void LobbyTick(PPCContext& ctx, uint8_t* base);
}
