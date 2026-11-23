#pragma once
#include <cstdint>
#include <string>
#include <rex/ppc/context.h>

// Game clips as files in a clips folder beside the exe: saved from the results screen, raced in Time Trials.
namespace cc2::clips {

void Start();
void Stop();
// Guest thread, every frame: the results screen's left stick prompt.
void Tick(PPCContext& ctx, uint8_t* base);

// NETMANXA sends its queue's front request: the Time Trial list's friends read becomes a read of the folder.
void Sending(uint8_t* base, uint32_t request);
void Sent();
// XamUserCreateStatsEnumerator made 'handle' inside that request: the size of the results it will write, else 0.
uint32_t Track(uint32_t handle, uint32_t kind, uint32_t rows, uint32_t specs, uint32_t view);
// XamEnumerate on such a handle: true when the request now waits for the folder scan.
bool Park(uint32_t handle, uint32_t buffer, uint32_t size, uint32_t overlapped);
// Game thread: answers the waiting requests whose scan is in.
void Complete();
void Cancel(uint32_t overlapped);
// The title storage GET for a listed row's clip: the file's bytes less the trailer.
bool Serve(const std::string& path, std::string* body);
// The Time Trial in play was launched with clips, which take the three ghost slots.
bool Racing();
// The 1000-byte X_AVATAR_METADATA is that of a clip the game was handed, so its build is a clip ghost's.
bool IsClipManifest(const uint8_t* manifest);

}  // namespace cc2::clips
