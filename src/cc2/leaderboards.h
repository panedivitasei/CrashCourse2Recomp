#pragma once
#include <cstdint>
#include <rex/ppc/context.h>

// Feeds the leaderboard page and course select's mini board speedrun.com level times (GET /v1/speedrun/<view id>).
namespace cc2::boards {

void Start();
void Stop();
// Guest thread, every frame: keeps the page's Y prompt hidden in the game's current Lua state.
void Tick(PPCContext& ctx, uint8_t* base);

// XamUserCreateStatsEnumerator made 'handle': returns the size of the results its enumeration will write, or 0 when
// the request is not one of the screens' and the caller's empty board stands.
uint32_t Track(uint32_t handle, uint32_t kind, uint64_t pivot, uint32_t rows, uint32_t specs, uint32_t view);
// XMsgCancelIORequest: the game frees a cancelled request's buffer, so it is never completed.
void Cancel(uint32_t overlapped);

}  // namespace cc2::boards
