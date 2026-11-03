#include "cc2_init.h"
#include "cc2/famestar.h"
#include "cc2/famestar_state.h"
#include "cc2/kernel/xam/local_players.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

REXCVAR_DEFINE_BOOL(cc2_famestar_local, true, "FameStar", "Use persistent local FameStar challenges and rank");

#ifdef DCC2_TU1
namespace cc2::fame {
namespace {
constexpr uint32_t kInvalid = 0x80070057u, kFailure = 0x80004005u;
std::mutex mutex;
State state;
std::filesystem::path save_path;
bool loaded = false, ready = false;
struct Award { std::string text; uint32_t points; };
std::deque<Award> awards;
uint64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
bool Commit(const State& next) {
  if (!next.Save(save_path)) {
    REXLOG_ERROR("[famestar] could not save {}; progress remains unchanged", save_path.string());
    return false;
  }
  state = next;
  return true;
}
bool Ensure() {
  if (!loaded) {
    auto* ks = REX_KERNEL_STATE();
    if (!ks || !ks->emulator() || ks->emulator()->user_data_root().empty()) return false;
    loaded = true;
    const auto uid = ks->user_profile()->xuid();
    save_path = ks->emulator()->user_data_root() / "famestar" / (std::to_string(uid) + ".dat");
    std::error_code ec;
    std::filesystem::create_directories(save_path.parent_path(), ec);
    if (ec) return false;
    const bool exists = std::filesystem::exists(save_path, ec);
    if (ec) return false;
    if (exists) ready = state.Load(save_path);
    else { state.week = Week(Now()); ready = state.Save(save_path); }
    if (!ready) {
      REXLOG_ERROR("[famestar] unavailable: could not read/create {}; existing data preserved", save_path.string());
      return false;
    }
    REXLOG_INFO("[famestar] local provider: {} points, rank {}, week {}, save={}",
                 state.Points(), state.Rank(), state.week, save_path.string());
  }
  if (!ready) return false;
  State next = state;
  if (next.Advance(Week(Now())) && !Commit(next)) return false;
  return true;
}
void Word(uint8_t* base, uint32_t p, uint32_t v) {
  *reinterpret_cast<rex::be<uint32_t>*>(base + p) = v;
}
void Wide(uint8_t* base, uint32_t p, uint32_t capacity, const std::string& text) {
  auto* out = reinterpret_cast<rex::be<uint16_t>*>(base + p);
  uint32_t n = std::min<uint32_t>(capacity - 1, static_cast<uint32_t>(text.size()));
  for (uint32_t i = 0; i < n; ++i) out[i] = static_cast<unsigned char>(text[i]);
  out[n] = 0;
}
void Result(PPCContext& ctx, uint32_t status) { ctx.r3.s64 = static_cast<int32_t>(status); }
void Time(uint8_t* base, uint32_t p) {
  uint64_t seconds = (uint64_t(state.week) + 1) * 604800ull - 3 * 86400ull;
  const auto now = Now();
  seconds = seconds > now ? seconds - now : 0;
  seconds = std::min<uint64_t>(seconds, 604800);
  Word(base, p, static_cast<uint32_t>(seconds / 86400));
  Word(base, p + 4, static_cast<uint32_t>(seconds / 3600 % 24));
  Word(base, p + 8, static_cast<uint32_t>(seconds / 60 % 60));
  Word(base, p + 12, static_cast<uint32_t>(seconds % 60));
}
}  // namespace
}  // namespace cc2::fame
using namespace cc2::fame;

REX_HOOK_RAW(sub_824FB5E0) {
  if (!REXCVAR_GET(cc2_famestar_local)) return __imp__sub_824FB5E0(ctx, base);
  const auto user = ctx.r3.u32, start = ctx.r4.u32, capacity = ctx.r5.u32;
  const auto count = ctx.r6.u32, output = ctx.r7.u32;
  if (!count || !output || user || start > kDefinitions.size()) return Result(ctx, kInvalid);
  Word(base, count, 0);
  std::lock_guard lock(mutex);
  if (!Ensure()) return Result(ctx, kFailure);
  const uint32_t n = std::min<uint32_t>(capacity, static_cast<uint32_t>(kDefinitions.size()) - start);
  for (uint32_t i = 0; i < n; ++i) {
    const auto& d = kDefinitions[start + i];
    const auto v = Describe(d, state.Count(d));
    const uint32_t p = output + i * 288;
    std::memset(base + p, 0, 288);
    Wide(base, p, 128, d.text);
    Word(base, p + 256, d.id);
    Word(base, p + 260, v.tiers);
    Word(base, p + 264, v.completed);
    Word(base, p + 268, v.progress);
    Word(base, p + 272, v.target);
    Word(base, p + 276, v.earned);
    Word(base, p + 280, v.maximum);
    base[p + 284] = d.weekly ? 1 : 0;
  }
  Word(base, count, n);
  REXLOG_DEBUG("[famestar] native challenge list: {} entries", n);
  Result(ctx, 0);
}

REX_HOOK_RAW(sub_824FB9A0) {
  if (!REXCVAR_GET(cc2_famestar_local)) return __imp__sub_824FB9A0(ctx, base);
  const auto user = ctx.r3.u32, id = ctx.r4.u32, delta = ctx.r5.u32;
  if (user || static_cast<int32_t>(delta) < 0) return Result(ctx, kInvalid);
  std::lock_guard lock(mutex);
  if (!Ensure()) return Result(ctx, kFailure);
  const auto* d = Find(id);
  if (!d || !delta) return Result(ctx, 0);
  State next = state;
  const auto earned = next.Add(id, delta);
  if (next.Count(*d) == state.Count(*d)) return Result(ctx, 0);
  if (!Commit(next)) return Result(ctx, kFailure);
  if (earned) awards.push_back({d->text, earned});
  REXLOG_DEBUG("[famestar] challenge {} progress={} earned={} total={}",
               id, state.Count(*d), earned, state.Points());
  Result(ctx, 0);
}

// Players 2-4 have no FameStar progress of their own: rank 0 under their own XUID, and no lobby counter (+32).
REX_HOOK_RAW(sub_824FAE70) {
  if (!REXCVAR_GET(cc2_famestar_local)) return __imp__sub_824FAE70(ctx, base);
  const auto user = ctx.r3.u32, p = ctx.r4.u32;
  const bool guest = user && user < 4 && cc2::kernel::xam::LocalPlayer(user);
  if ((user && !guest) || !p) return Result(ctx, kInvalid);
  std::memset(base + p, 0, 56);
  std::lock_guard lock(mutex);
  if (!Ensure()) return Result(ctx, kFailure);
  const auto rank = guest ? 0u : state.Rank(), points = guest ? 0u : state.Points();
  const auto next = kRankThresholds[std::min<uint32_t>(rank + 1, 5)];
  *reinterpret_cast<rex::be<uint64_t>*>(base + p) =
      guest ? cc2::kernel::xam::LocalPlayerXuid(user) : REX_KERNEL_STATE()->user_profile()->xuid();
  Word(base, p + 8, rank);
  Word(base, p + 12, guest ? 0u : state.acknowledged_rank);
  Word(base, p + 24, points);
  Word(base, p + 28, points < next ? next - points : 0);
  Word(base, p + 32, guest ? 0u : next);
  // The native status bar divides remaining points by this per-rank span.
  for (uint32_t i = 0; i < 5; ++i) Word(base, 0x82DA03C8u + i * 4, kRankThresholds[i + 1] - kRankThresholds[i]);
  Result(ctx, 0);
}

REX_HOOK_RAW(sub_824FB040) {
  if (!REXCVAR_GET(cc2_famestar_local)) return __imp__sub_824FB040(ctx, base);
  if (ctx.r3.u32) return Result(ctx, kInvalid);
  std::lock_guard lock(mutex);
  if (!Ensure()) return Result(ctx, kFailure);
  State next = state;
  next.acknowledged_rank = next.Rank();
  Result(ctx, next.acknowledged_rank == state.acknowledged_rank || Commit(next) ? 0 : kFailure);
}

REX_HOOK_RAW(sub_824FB1B0) {
  if (!REXCVAR_GET(cc2_famestar_local)) return __imp__sub_824FB1B0(ctx, base);
  const auto user = ctx.r3.u32, p = ctx.r4.u32;
  if (user || !p) return Result(ctx, kInvalid);
  std::memset(base + p, 0, 264);
  std::lock_guard lock(mutex);
  if (!Ensure()) return Result(ctx, kFailure);
  if (awards.empty()) return Result(ctx, 1);
  const auto award = awards.front();
  awards.pop_front();
  Wide(base, p, 128, award.text);
  Word(base, p + 256, award.points);
  Result(ctx, 0);
}

REX_HOOK_RAW(sub_824FC088) {
  if (!REXCVAR_GET(cc2_famestar_local)) return __imp__sub_824FC088(ctx, base);
  const auto rank = ctx.r3.u32, capacity = ctx.r4.u32, p = ctx.r5.u32;
  if (rank > 5 || !capacity || !p) return Result(ctx, kInvalid);
  Wide(base, p, capacity, kRankNames[rank]);
  Result(ctx, 0);
}

REX_HOOK_RAW(sub_824FB4B0) {
  if (!REXCVAR_GET(cc2_famestar_local)) return __imp__sub_824FB4B0(ctx, base);
  const auto p = ctx.r3.u32;
  if (!p) return Result(ctx, kInvalid);
  std::lock_guard lock(mutex);
  if (!Ensure()) return Result(ctx, kFailure);
  Time(base, p);
  Result(ctx, 0);
}

REX_HOOK_RAW(sub_824FB3D0) {
  if (!REXCVAR_GET(cc2_famestar_local)) return __imp__sub_824FB3D0(ctx, base);
  if (ctx.r3.u32 || !ctx.r4.u32) return Result(ctx, kInvalid);
  std::memset(base + ctx.r4.u32, 0, 16);
  Result(ctx, 0);
}

REX_HOOK_RAW(sub_824FBE68) {
  if (!REXCVAR_GET(cc2_famestar_local)) return __imp__sub_824FBE68(ctx, base);
  const auto user = ctx.r3.u32, callback = ctx.r4.u32, parameter = ctx.r5.u32;
  if (user || !callback) return Result(ctx, kInvalid);
  {
    std::lock_guard lock(mutex);
    if (!Ensure()) return Result(ctx, kFailure);
  }
  // Native flush also completes synchronously when its pending buffer is empty.
  const PPCContext saved = ctx;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = parameter;
  REX_CALL_INDIRECT_FUNC(callback);
  ctx = saved;
  Result(ctx, 0);
}
REX_HOOK_RAW(sub_824FA810) {
  if (!REXCVAR_GET(cc2_famestar_local)) return __imp__sub_824FA810(ctx, base);
  const auto user = ctx.r3.u32, query = ctx.r4.u32, entries = ctx.r6.u32;
  const auto results = ctx.r7.u32, operation = ctx.r8.u32;
  const auto callback = ctx.r9.u32, parameter = ctx.r10.u32;
  if (operation) Word(base, operation, 0);
  if (user || !query || !entries || !results || (operation && !callback)) return Result(ctx, kInvalid);
  // There is no local equivalent of the cross-game global/friends leaderboard.
  Word(base, results, UINT32_MAX);
  Word(base, results + 4, 0);
  Word(base, results + 8, 0);
  if (callback) {
    const PPCContext saved = ctx;
    ctx.r3.u64 = 0;
    ctx.r4.u64 = parameter;
    REX_CALL_INDIRECT_FUNC(callback);
    ctx = saved;
  }
  REXLOG_DEBUG("[famestar] offline leaderboard: empty result");
  Result(ctx, 0);
}
extern "C" REX_FUNC(__imp__sub_82AF0D98);  // starts a gamepad profile's read: state 1, then 2, or 0 on failure

// Lua handler (gamepad index), the lobby's fame-star-info check: true once the gamepad's profile read state
// (profile+5832) is 2 and, on Live, sub_824FAE70 answers for it. Restarts the read of a signed-in player idle at 0
// for 1 s, up to 3 times until it reaches 2: the game starts none for players 2-4 and does not retry a failed one.
REX_HOOK_RAW(sub_82BD13E8) {
  uint32_t states[4];
  const uint32_t table = *reinterpret_cast<rex::be<uint32_t>*>(base + 0x82EAA320u);
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t profile = table ? uint32_t(*reinterpret_cast<rex::be<uint32_t>*>(base + table + i * 4)) : 0u;
    states[i] = profile ? uint32_t(*reinterpret_cast<rex::be<uint32_t>*>(base + profile + 5832)) : UINT32_MAX;
  }
  using Clock = std::chrono::steady_clock;
  static Clock::time_point idle_since[4] = {};
  static uint32_t attempts[4] = {};
  const auto now = Clock::now();
  for (uint32_t i = 0; i < 4 && table; ++i) {
    if (i && !cc2::kernel::xam::LocalPlayer(i)) {
      attempts[i] = 0;
      idle_since[i] = {};
      continue;
    }
    if (states[i] == 2) attempts[i] = 0;
    if (states[i] != 0 || attempts[i] >= 3) {
      idle_since[i] = {};
      continue;
    }
    if (idle_since[i] == Clock::time_point{}) idle_since[i] = now;
    if (now - idle_since[i] < std::chrono::seconds(1)) continue;
    idle_since[i] = {};
    ++attempts[i];
    const uint32_t profile = *reinterpret_cast<rex::be<uint32_t>*>(base + table + i * 4);
    const uint32_t index = profile ? uint32_t(*reinterpret_cast<rex::be<uint32_t>*>(base + profile + 4)) : ~0u;
    if (index != i) {
      REXLOG_WARN("[lobby] player {}'s profile object {:#x} names gamepad {}; read not started", i + 1, profile,
                  int32_t(index));
      continue;
    }
    const PPCContext saved = ctx;
    ctx.r3.u64 = profile;
    __imp__sub_82AF0D98(ctx, base);
    ctx = saved;
    REXLOG_INFO("[lobby] restarted player {}'s profile read (attempt {})", i + 1, attempts[i]);
  }
  __imp__sub_82BD13E8(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_82312F10);  // CallScriptOnObj(*r4, object *r5, "megastar_counter_workaround", *r6, *r7)

namespace cc2::fame {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kCounterDelay = std::chrono::milliseconds(750);
// A lobby slot's FameStar counter held back until the slot has slid in: the slot's GUI element handle (index, id),
// its gamertag and counter.
struct PendingCounter {
  int16_t index;
  uint64_t id;
  std::string gamertag;
  uint32_t value;
  Clock::time_point due;
};
std::vector<PendingCounter> pending_counters;
std::vector<uint64_t> placed_counters;  // element ids whose counter was sent
// The GUI element behind a handle, as FillInGamertag resolves it: table 0x82E993F4, slot index + 395, id at +16.
uint32_t LobbyElement(uint8_t* base, int16_t index, uint64_t id) {
  if (index < 0 || index >= 2047) return 0;
  const uint32_t table = *reinterpret_cast<rex::be<uint32_t>*>(base + 0x82E993F4u);
  const uint32_t element =
      table ? uint32_t(*reinterpret_cast<rex::be<uint32_t>*>(base + table + (uint32_t(index) + 395) * 4)) : 0u;
  return element && uint64_t(*reinterpret_cast<rex::be<uint64_t>*>(base + element + 16)) == id ? element : 0u;
}
}  // namespace

void LobbyTick(PPCContext& ctx, uint8_t* base) {
  if (pending_counters.empty()) return;
  const auto now = Clock::now();
  for (auto it = pending_counters.begin(); it != pending_counters.end();) {
    if (now < it->due) {
      ++it;
      continue;
    }
    const uint32_t element = LobbyElement(base, it->index, it->id);
    const uint32_t lua = *reinterpret_cast<rex::be<uint32_t>*>(base + 0x82EDD490u);
    if (element && lua) {
      // Arguments staged below the guest stack pointer: name, object, gamertag pointer, counter, gamertag bytes.
      const uint32_t block = (ctx.r1.u32 - 0x300u) & ~0xFu;
      Word(base, block, 0x8212D55Cu);  // "CallScriptOnObj"
      Word(base, block + 4, element);
      Word(base, block + 8, block + 16);
      Word(base, block + 12, it->value);
      std::memcpy(base + block + 16, it->gamertag.c_str(), it->gamertag.size() + 1);
      const PPCContext saved = ctx;
      ctx.r1.u32 = block - 0x40u;
      ctx.r3.u64 = lua;
      ctx.r4.u64 = block;
      ctx.r5.u64 = block + 4;
      ctx.r6.u64 = block + 8;
      ctx.r7.u64 = block + 12;
      __imp__sub_82312F10(ctx, base);
      ctx = saved;
      placed_counters.push_back(it->id);
    }
    it = pending_counters.erase(it);
  }
}
}  // namespace cc2::fame

// FillInGamertag's counter call (0x82BD176C): a slot's first counter goes out as 0 and LobbyTick sends the real one
// kCounterDelay later. The lobby script places the counter 3 frames after it changes, at the gamertag's measured
// width, which reads 0 while a newly joined slot is still sliding in.
REX_HOOK_RAW(sub_82312F10) {
  if (uint32_t(ctx.lr) != 0x82BD176Cu) return __imp__sub_82312F10(ctx, base);
  const uint32_t value_slot = ctx.r7.u32, value = *reinterpret_cast<rex::be<uint32_t>*>(base + value_slot);
  const int16_t index = int16_t(uint16_t(*reinterpret_cast<rex::be<uint16_t>*>(base + ctx.r1.u32 + 116)));
  const uint64_t id = *reinterpret_cast<rex::be<uint64_t>*>(base + ctx.r1.u32 + 112);
  auto it = std::find_if(pending_counters.begin(), pending_counters.end(),
                         [id](const PendingCounter& p) { return p.id == id; });
  if (it == pending_counters.end()) {
    if (!value || std::find(placed_counters.begin(), placed_counters.end(), id) != placed_counters.end()) {
      return __imp__sub_82312F10(ctx, base);
    }
    pending_counters.push_back({index, id, {}, 0, Clock::now() + kCounterDelay});
    it = std::prev(pending_counters.end());
  }
  const uint32_t name = *reinterpret_cast<rex::be<uint32_t>*>(base + ctx.r6.u32);
  it->gamertag.clear();
  for (uint32_t i = 0; name && i < 15 && base[name + i]; ++i) it->gamertag += char(base[name + i]);
  it->value = value;
  Word(base, value_slot, 0);
  __imp__sub_82312F10(ctx, base);
  Word(base, value_slot, value);
}

extern "C" REX_FUNC(__imp__sub_828926A8);  // XMarketplaceCreateAssetEnumerator(user, count, &buffer_size, &handle)
extern "C" REX_FUNC(__imp__sub_8229E758);  // coin sync step: enumerates the profile's marketplace assets into +32 -> +72

namespace {
bool asset_enumerator_failed = false;
}

// XamCreateEnumeratorHandle fails, leaving the buffer size and handle unwritten; the caller ignores the error. Writes
// the size for count assets and a null handle.
REX_HOOK_RAW(sub_828926A8) {
  const uint32_t count = ctx.r4.u32, size_out = ctx.r5.u32, handle_out = ctx.r6.u32;
  __imp__sub_828926A8(ctx, base);
  asset_enumerator_failed = ctx.r3.u32 != 0;
  if (!asset_enumerator_failed) return;
  if (size_out) Word(base, size_out, (count + 34) * 8);
  if (handle_out) Word(base, handle_out, 0);
}

// With no enumerator the step's buffer stays uninitialised and a nonzero asset sends the profile read to a coin
// purchase order with no Shop offer, failing the read. Zeroes the one-asset buffer and completes the overlapped (+4)
// with ERROR_NO_MORE_FILES.
REX_HOOK_RAW(sub_8229E758) {
  const uint32_t step = ctx.r3.u32;
  asset_enumerator_failed = false;
  __imp__sub_8229E758(ctx, base);
  if (!asset_enumerator_failed) return;
  const uint32_t context = *reinterpret_cast<rex::be<uint32_t>*>(base + step + 32);
  const uint32_t buffer = context ? uint32_t(*reinterpret_cast<rex::be<uint32_t>*>(base + context + 72)) : 0u;
  if (buffer) std::memset(base + buffer, 0, (1 + 34) * 8);
  Word(base, step + 4, 0);
  Word(base, step + 28, 0x80070012u);
}

REX_HOOK_RAW(sub_82AECD80) {
  if (!REXCVAR_GET(cc2_famestar_local)) return __imp__sub_82AECD80(ctx, base);
  bool available;
  {
    std::lock_guard lock(mutex);
    available = Ensure();
  }
  // TelemetryXA::IsOnline supplies the Lua boolean used by the FameStar entry button.
  const PPCContext saved = ctx;
  const uint32_t result = ctx.r3.u32;
  const uint32_t scratch = (ctx.r1.u32 - 0x80u) & ~15u;
  Word(base, result + 4, 0);
  Word(base, scratch + 0x50, 6);
  ctx.r1.u32 = scratch;
  ctx.r4.u64 = scratch + 0x50;
  ctx.r5.u64 = result;
  ctx.r6.u64 = available ? 1 : 0;
  sub_82242478(ctx, base);
  ctx = saved;
  ctx.r3.u64 = result;
}

REX_HOOK_RAW(sub_829D1500) {
  if (!REXCVAR_GET(cc2_famestar_local)) return __imp__sub_829D1500(ctx, base);
  const uint32_t text = ctx.r5.u32, speed = ctx.r6.u32;
  bool available;
  {
    std::lock_guard lock(mutex);
    available = Ensure();
  }
  if (!available) return __imp__sub_829D1500(ctx, base);
  Wide(base, text, 512, "Welcome back to Doritos Crash Course 2! We hope you enjoy the game!");
  Word(base, speed, 0x3F800000u);
  Result(ctx, 0);
}

REX_HOOK_RAW(sub_82B8D3E8) {
  if (!REXCVAR_GET(cc2_famestar_local)) return __imp__sub_82B8D3E8(ctx, base);
  bool available;
  {
    std::lock_guard lock(mutex);
    available = Ensure();
  }
  const uint32_t telemetry = *reinterpret_cast<rex::be<uint32_t>*>(base + 0x82EDC34Cu);
  if (!available || !telemetry) return __imp__sub_82B8D3E8(ctx, base);
  // The automatic FameStar popup checks remote status directly; preserve it outside this UI call.
  const uint32_t remote_status = *reinterpret_cast<rex::be<uint32_t>*>(base + telemetry + 4);
  Word(base, telemetry + 4, 0);
  __imp__sub_82B8D3E8(ctx, base);
  Word(base, telemetry + 4, remote_status);
}

#else
namespace cc2::fame {
void LobbyTick(PPCContext&, uint8_t*) {}
}  // namespace cc2::fame
#endif
