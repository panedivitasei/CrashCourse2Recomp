// Presented-frame and draw counters, and the guest frame rate for the F3 overlay.

#include "cc2/scene_driver.h"

#include <atomic>
#include <chrono>
#include <mutex>

#include <rex/ppc/context.h>

namespace cc2::scene {

namespace {

std::atomic<uint32_t> g_frame{0};
std::atomic<uint32_t> g_draws{0};
std::atomic<uint32_t> g_draws_last{0};
std::mutex g_stats_mutex;
rex::ui::FrameStats g_stats;
std::chrono::steady_clock::time_point g_last_swap;

}  // namespace

void OnSwap() {
  const uint32_t frame = g_frame.fetch_add(1, std::memory_order_acq_rel) + 1;
  g_draws_last.store(g_draws.exchange(0, std::memory_order_acq_rel), std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_stats_mutex);
  if (frame > 1) {
    const double ms = std::chrono::duration<double, std::milli>(now - g_last_swap).count();
    g_stats.frame_time_ms = g_stats.frame_time_ms == 0 ? ms : g_stats.frame_time_ms * 0.9 + ms * 0.1;
    g_stats.fps = g_stats.frame_time_ms > 0 ? 1000.0 / g_stats.frame_time_ms : 0;
  }
  g_stats.frame_count = frame;
  g_last_swap = now;
}

void OnDraw() { g_draws.fetch_add(1, std::memory_order_relaxed); }

uint32_t LastFrameDraws() { return g_draws_last.load(std::memory_order_relaxed); }

uint64_t FrameCount() { return g_frame.load(std::memory_order_acquire); }

rex::ui::FrameStats GuestFrameStats() {
  std::lock_guard lock(g_stats_mutex);
  return g_stats;
}

}  // namespace cc2::scene

// D3DDevice_BeginVertices / BeginIndexedVertices entries; the other draw entries count from their own hooks.
void Cc2Scene_CountDraw() { cc2::scene::OnDraw(); }
