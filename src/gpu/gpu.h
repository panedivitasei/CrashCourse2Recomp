// Native renderer entry points used by the app shell, the capture hooks and the scene driver.
#pragma once

#include <cstdint>
#include <vector>
#include <filesystem>

#include "gpu/gpu_records.h"

namespace rex::ui {
class Window;
}  // namespace rex::ui

namespace cc2::gpu {

// Creates the plume device and swap chain on the window and starts the render thread; false leaves the renderer off.
bool Initialize(rex::ui::Window* window);

// Stops the render thread, idles the device and destroys everything in reverse order; safe to call twice.
void Shutdown();

// The window's pixel size changed; the render thread rebuilds the swap chain before its next present.
void RequestResize();

// Guest thread: appends a record to the frame being built.
void Submit(Record record);

// Guest thread: hands the frame to the render thread, waiting while two frames are already queued.
void EndFrame();

// Folder holding the game's cardigan_bd font, used by the gpu_fps counter drawn at present.
void SetOverlayFontRoot(const std::filesystem::path& folder);

// Frames handed to the render thread so far; the capture keys per-frame upload coalescing on it.
uint64_t FrameIndex();

// Range keys the host dropped from shared memory since the last call; the guest thread forgets it sent them.
void TakeEvicted(std::vector<uint64_t>& keys);
bool AnyEvicted();

// First-frame hold until the start-up pipeline warm-up has compiled (bounded by gpu_pipeline_warmup_wait).
void WaitForWarmup();

// Vertical sync on the swap chain, applied by the host thread at its next present.
void SetVsync(bool enabled);

// The mouse pointer the host draws over the presented frame: position as a 0-1 fraction of the game image.
void SetPointer(float x, float y, bool visible);
// True while the game draws the hand itself (cc2::mouse::GloveTick); the baked sprite is skipped then.
void SuppressPointer(bool suppressed);

namespace draw {
// Guest thread: submits a Begin*Vertices draw whose End the game inlined; every commit point calls it first.
void FinishPending();
}  // namespace draw

}  // namespace cc2::gpu
