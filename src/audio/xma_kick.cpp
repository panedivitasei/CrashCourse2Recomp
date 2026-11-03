// XMA decoder wake, title-side.
//
// The guest frees output-ring space with plain memory writes, which hardware
// notices; the runtime's decoder worker only wakes on a context kick and
// otherwise blocks with no timeout. DCC2's media stack then spins on the ring
// cursor on the main thread until an unrelated sound kicks the decoder,
// stalling the menu for seconds. A 1 ms thread kicks it from outside the SDK.
//
// The SDK's kick is private; writing a context-kick register with an empty
// mask reaches the same signal with both kick loops skipped.

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/audio/audio_system.h>
#include <rex/audio/xma/decoder.h>
#include <rex/audio/xma/register_file.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

namespace rex::audio {

namespace {

XmaDecoder* LiveDecoder() {
  Runtime* runtime = Runtime::instance();
  auto* kernel_state = runtime ? runtime->kernel_state() : nullptr;
  auto* emulator = kernel_state ? kernel_state->emulator() : nullptr;
  auto* audio_system = emulator ? static_cast<AudioSystem*>(emulator->audio_system()) : nullptr;
  return audio_system ? audio_system->xma_decoder() : nullptr;
}

std::atomic<bool> g_kicker_run{false};
std::thread g_kicker;

}  // namespace

void KickXmaDecoder() {
  if (auto* decoder = LiveDecoder()) {
    decoder->WriteRegister(XmaRegister::Context0Kick * 4, 0);
  }
}

// Sweep the decoder every millisecond for as long as the title runs.
void StartXmaKicker() {
  if (g_kicker_run.exchange(true)) {
    return;
  }
  g_kicker = std::thread([] {
    while (g_kicker_run.load(std::memory_order_relaxed)) {
      KickXmaDecoder();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  REXLOG_DEBUG("[xma] 1 ms decoder kicker running");
}

// Must run before the audio system goes away: the kick dereferences it.
void StopXmaKicker() {
  if (!g_kicker_run.exchange(false)) {
    return;
  }
  if (g_kicker.joinable()) {
    g_kicker.join();
  }
}

}  // namespace rex::audio
