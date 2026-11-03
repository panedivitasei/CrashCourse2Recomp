#include "cc2_init.h"
#include "cc2/jinx_wearables_state.h"
#include <array>
#include <chrono>
#include <mutex>
#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(cc2_jinx_wearables, true, "Gameplay", "Show equipped Jinx models on their owners");

#ifdef DCC2_TU1
namespace cc2::jinx {
namespace {
constexpr uint32_t kGame = 0x82EAA328, kScene = 0x82EDBB1C;
constexpr uint32_t kKernel = 0x82EDBB14, kResources = 0x82EDBAF0;
using Clock = std::chrono::steady_clock;
struct Wearable {
  uint32_t actor = 0, resource = 0, bank = 0, original_mask = 0, own_mask = 0;
  uint64_t actor_id = 0, avatar_id = 0;
  bool applied = false, pending = false;
  Clock::time_point retry{};
};
std::array<Wearable, 4> wearables;
WearableRequests requests;
std::recursive_mutex mutex;
thread_local const WearableRequest* loading = nullptr;
uint32_t Read(uint8_t* base, uint32_t p) { return *reinterpret_cast<rex::be<uint32_t>*>(base + p); }
uint64_t Read64(uint8_t* base, uint32_t p) { return *reinterpret_cast<rex::be<uint64_t>*>(base + p); }
void Word(uint8_t* base, uint32_t p, uint32_t v) { *reinterpret_cast<rex::be<uint32_t>*>(base + p) = v; }
bool Race(uint8_t* base, uint32_t game) {
  return game && (Read(base, game + 124) == 2 || Read(base, game + 124) == 3);
}
bool Jinxed(uint8_t* base, uint32_t game, uint32_t slot) {
  return *reinterpret_cast<rex::be<float>*>(base + game + 196 + slot * 64) > 0.0f;
}
uint32_t Fighter(PPCContext& ctx, uint8_t* base, uint32_t game, uint32_t slot) {
  ctx.r3.u32 = game; ctx.r4.u32 = slot;
  sub_82B6A3B0(ctx, base);
  return ctx.r3.u32;
}
uint32_t Equipped(PPCContext& ctx, uint8_t* base, uint32_t game, uint32_t slot) {
  const auto player = Read(base, game + 172 + slot * 64);
  const auto manager = Read(base, kResources);
  if (!player || !manager) return 0;
  // The profile stores a resource handle, not the JINXMASTER UniqueId.
  ctx.r3.u32 = manager; ctx.r4.u64 = Read64(base, player + 5760);
  const auto resolve = Read(base, Read(base, manager) + 168);
  REX_CALL_INDIRECT_FUNC(resolve);
  return ctx.r3.u32;
}

bool Alive(PPCContext& ctx, uint8_t* base, const Wearable& w) {
  const auto scene = Read(base, kScene);
  if (!scene || !w.actor) return false;
  ctx.r3.u32 = scene + 54044; ctx.r4.u64 = w.actor_id;
  sub_82326638(ctx, base);
  return ctx.r3.u32 == w.actor;
}
uint32_t Avatar(PPCContext& ctx, uint8_t* base, uint32_t actor) {
  ctx.r3.u32 = Read(base, kResources); ctx.r4.u64 = Read64(base, actor + 712);
  sub_82AD71F8(ctx, base);
  return ctx.r3.u32;
}
void Mask(PPCContext& ctx, uint8_t* base, uint32_t avatar, uint32_t mask) {
  if (Read(base, avatar + 704) == mask) return;
  Word(base, avatar + 704, mask);
  ctx.r3.u32 = avatar; sub_829ECC48(ctx, base);
  ctx.r3.u32 = avatar; sub_829EB7C8(ctx, base);
}
void Remove(PPCContext& ctx, uint8_t* base, uint32_t slot) {
  auto& w = wearables[slot];
  requests.Cancel(slot);
  if (w.applied && Alive(ctx, base, w)) {
    if (Read(base, w.actor + 1396) == w.bank) {
      ctx.r3.u32 = w.actor; ctx.r4.u32 = 1; ctx.r5.u32 = 0;
      sub_82255220(ctx, base);
    }
    if (Read64(base, w.actor + 712) == w.avatar_id) {
      const auto avatar = Avatar(ctx, base, w.actor);
      if (avatar && Read(base, avatar + 704) == w.own_mask) Mask(ctx, base, avatar, w.original_mask);
    }
  }
  w = {};
}
void Start(PPCContext& ctx, uint8_t* base, uint32_t game, uint32_t slot,
           uint32_t actor, uint32_t resource) {
  auto& w = wearables[slot];
  const auto avatar = Avatar(ctx, base, actor);
  if (!avatar) return;
  w.actor = actor; w.actor_id = Read64(base, actor + 16); w.resource = resource;
  w.avatar_id = Read64(base, actor + 712); w.original_mask = Read(base, avatar + 704);
  w.own_mask = Read(base, resource + 92); w.pending = true;
  w.retry = Clock::now() + std::chrono::seconds(2);
  const auto request = requests.Begin(slot, game, actor, w.actor_id, resource);
  REXLOG_DEBUG("[jinx-wearable] loading slot {} actor {:#x} resource {:#x}", slot, actor, resource);
  const uint32_t stack = ctx.r1.u32;
  const uint32_t result = (stack - 0x100) & ~15u;
  ctx.r1.u32 = result - 0x80;
  Word(base, result, 0); Word(base, result + 4, 0);
  // Use the native preview loader and its weak actor handle; keep the owner's vanity bank intact.
  loading = &request;
  ctx.r3.u32 = result; ctx.r4.u32 = resource; ctx.r5.u32 = actor;
  sub_82BCDFD0(ctx, base);
  loading = nullptr;
  if (Read(base, result)) {
    ctx.r3.u32 = Read(base, kKernel) + 608; ctx.r4.u32 = result; ctx.r5.u32 = 0;
    sub_82230000(ctx, base);
  } else {
    requests.Cancel(slot); w.pending = false;
    REXLOG_WARN("[jinx-wearable] preview unavailable for slot {} resource {:#x}", slot, resource);
  }
  ctx.r3.u32 = result + 4; sub_8223DAB8(ctx, base);
  ctx.r1.u32 = stack;
}
void Tick(PPCContext& ctx, uint8_t* base, uint32_t game) {
  std::lock_guard lock(mutex);
  const PPCContext saved = ctx;
  for (uint32_t slot = 0; slot < wearables.size(); ++slot) {
    auto& w = wearables[slot];
    if (!REXCVAR_GET(cc2_jinx_wearables) || !Race(base, game)) { Remove(ctx, base, slot); continue; }
    const auto actor = Fighter(ctx, base, game, slot);
    const auto resource = actor ? Equipped(ctx, base, game, slot) : 0;
    if (w.actor && (actor != w.actor || !actor || Read64(base, actor + 16) != w.actor_id ||
        Read64(base, actor + 712) != w.avatar_id || resource != w.resource)) Remove(ctx, base, slot);
    if (!actor || !resource || Jinxed(base, game, slot)) continue;
    if (w.applied && Read(base, actor + 1396) != w.bank) Remove(ctx, base, slot);
    if (!w.applied && !w.pending && Clock::now() >= w.retry) Start(ctx, base, game, slot, actor, resource);
  }
  ctx = saved;
}
}  // namespace
}  // namespace cc2::jinx
using namespace cc2::jinx;

REX_HOOK_RAW(sub_82B7C9C8) {
  const auto game = ctx.r3.u32;
  __imp__sub_82B7C9C8(ctx, base);
  Tick(ctx, base, game);
}

REX_HOOK_RAW(sub_82255220) {
  if (loading && ctx.r3.u32 == loading->actor && ctx.r4.u32 == 2 && !ctx.r5.u32) return;
  __imp__sub_82255220(ctx, base);
}

REX_HOOK_RAW(sub_8226A280) {
  const auto output = ctx.r3.u32;
  __imp__sub_8226A280(ctx, base);
  std::lock_guard lock(mutex);
  requests.Bind(Read(base, output), loading);
}

REX_HOOK_RAW(sub_8226A1B8) {
  const auto task = ctx.r3.u32;
  std::unique_lock lock(mutex);
  const auto request = requests.Take(task);
  if (!request) { lock.unlock(); return __imp__sub_8226A1B8(ctx, base); }
  const auto& r = *request;
  const PPCContext saved = ctx;
  auto& w = wearables[r.slot];
  const bool current = requests.Current(r);
  if (current) w.pending = false;
  const bool valid = current && REXCVAR_GET(cc2_jinx_wearables) && Read(base, kGame) == r.game &&
      Race(base, r.game) && !Jinxed(base, r.game, r.slot) &&
      Equipped(ctx, base, r.game, r.slot) == r.resource && Fighter(ctx, base, r.game, r.slot) == r.actor &&
      Alive(ctx, base, w) && Read64(base, r.actor + 712) == w.avatar_id &&
      Read64(base, task + 8) == r.actor_id && Read(base, task + 24) == r.resource;
  ctx = saved;
  if (!valid) { ctx.r3.u32 = 2; return; }
  w.bank = Read(base, task + 20);
  __imp__sub_8226A1B8(ctx, base);
  w.applied = Read(base, r.actor + 1396) == w.bank && w.bank;
  if (w.applied) REXLOG_DEBUG("[jinx-wearable] attached slot {} actor {:#x} resource {:#x}", r.slot, r.actor, r.resource);
}

REX_HOOK_RAW(sub_8228BEF8) {
  { std::lock_guard lock(mutex); requests.Destroy(ctx.r3.u32); }
  __imp__sub_8228BEF8(ctx, base);
}

REX_HOOK_RAW(sub_82B77458) {
  const auto slot = ctx.r5.u32;
  if (slot < wearables.size()) {
    std::lock_guard lock(mutex);
    const PPCContext saved = ctx;
    // Restore the base mask before the native attack snapshots it for expiry and rewind.
    Remove(ctx, base, slot);
    ctx = saved;
  }
  __imp__sub_82B77458(ctx, base);
}
#endif
