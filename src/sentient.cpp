// =====================================================================
//  sentient.cpp -- report the offline Sentient server as connected
//
//  The title's online backend ("Sentient", Beenox) gates the front end on a
//  server connection. Sentient::SentientUpdate (sub_824ED7C8) returns the
//  connection state produced by SenSys::Update (sub_824ED520):
//  0x580005 = connection failed, 0x580004 = connected (callers also accept
//  0x580003 and 0).
//
//  Reporting 0x580004 passes the content-connection retry gate
//  (common/ui/men/splash/content_connection_error). Server-gated entry logic
//  is satisfied; actual Sentient I/O still fails.
// =====================================================================
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Resolved via REXGLUE_ENTRYPOINT_INCLUDE_DIR (generated/default or default_tu1).
#include "cc2_init.h"
#include "cc2/display_settings.h"
#include "cc2/famestar.h"
#include "cc2/lobby.h"
#include "cc2/mouse_ui.h"
#include "dcc2_tu1_map.h"  // base<->TU1 guest address selection (must follow cc2_init.h)
#include <rex/cvar.h>
#include <rex/exception_handler.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include "cc2/kernel/xam/lg_economy_file.h"
#include "cc2/kernel/xam/local_players.h"
#include "cc2/thread/precise_sleep.h"  // media-wait spin relief

// game_data_root is defined and exported by the runtime (runtime.cpp). The economy file format is
// shared via cc2/kernel/xam/lg_economy_file.h; the runtime's LG server (src/kernel/xam/xhttp.cpp,
// dcc2_lg_start_stars) creates the file.
REXCVAR_DECLARE(std::string, game_data_root);

REXCVAR_DEFINE_BOOL(dcc2_water_reflection, true, "DCC2/Perf",
                    "Planar water reflections (the whole scene rendered a second time, "
                    "mirrored, into the ReflectionPass RTT). Set false on weak GPUs: water "
                    "still renders and perturbs, its mirror image just goes flat. Pairs "
                    "well with a lower internal resolution (set the 'resolution' cvar to "
                    "e.g. 848x480 or 480p).");
REXCVAR_DEFINE_BOOL(dcc2_minimal_render, false, "DCC2/Perf",
                    "Legacy content-culling performance mode: skip background scenery "
                    "actors/models/animated decor/fx/sub-mesh parts and the water pass per "
                    "the dcc2_performance_cull* lists. Faster on weak CPUs; the backdrop goes "
                    "empty. Deprecated; prefer dcc2_water_reflection and the 'resolution' "
                    "cvar.");
REXCVAR_DEFINE_DOUBLE(dcc2_render_distance, 0.0, "DCC2/Perf",
                      "Camera far-plane clamp in world units (0 = game default). Geometry "
                      "beyond this distance is clipped -- the distant backdrop disappears "
                      "and stops costing GPU time. Perspective cameras only (shadow maps "
                      "unaffected). Live-tunable from the F4 menu; try 1000-3000.");
// Shadow box floor. CAMERA__UpdateMatrices_ShadowCam builds the sun ortho box from a per-level
// world bound (half-extents at r1+0x70/74/78, already halved). Levels with a small box
// (Antarctic) lose shadows once the caster leaves it. Each axis is raised to at least this
// half-extent in world units; larger boxes are untouched. 0 = axis untouched. Live-tunable (F4).
REXCVAR_DEFINE_DOUBLE(dcc2_shadow_min_ext_x, 40.0, "DCC2/Perf",
                      "Shadow ortho box min half-extent, axis X (0 = untouched).");
REXCVAR_DEFINE_DOUBLE(dcc2_shadow_min_ext_y, 0.0, "DCC2/Perf",
                      "Shadow ortho box min half-extent, axis Y (0 = untouched).");
REXCVAR_DEFINE_DOUBLE(dcc2_shadow_min_ext_z, 40.0, "DCC2/Perf",
                      "Shadow ortho box min half-extent, axis Z (0 = untouched).");
// Shadow draw distance. SCENEXA::GetShadowBounds returns farSlice = 0.995: the shadow ortho window
// fits the main camera frustum truncated at that NDC depth. NDC->world depends on near/far
// (d = f*n/(f - slice*(f-n))), so 0.995 is ~20u at near = 0.1 (Antarctic) and much farther on
// levels with a larger near plane. The hook solves for the slice that reaches this world distance
// and only raises it. A larger window spreads the 1024^2 map thinner (softer shadows).
REXCVAR_DEFINE_DOUBLE(dcc2_shadow_distance, 60.0, "DCC2/Perf",
                      "Shadow coverage distance in world units (0 = game default). "
                      "Extends how far from the camera shadows stay rendered; "
                      "larger = softer shadows. Live-tunable; try 40-150.");
REXCVAR_DEFINE_STRING(dcc2_performance_cull_fx,
                      "waterfall_steam,/mist,lightning_bug,pyramid_ray,torch_f,"
                      "torch_smoke,splash_hi,fx/ray/,directional_ray,torch_glow,"
                      "statue_eye_flames,waterfall_splash", "DCC2/Perf",
                      "Comma-separated case-insensitive substrings; performance mode declines "
                      "creating particle/FX-family entities whose instance name matches. "
                      "Separate from dcc2_performance_cull: some engine code retry-loops on a "
                      "declined entity (startup hang). Empty = no entity culling.");
REXCVAR_DEFINE_STRING(dcc2_performance_cull,
                      "/on/,/ch/,ab/reef,ab/chunks/mountain,oa/ruin,oa/head_statue,"
                      "oa/toltec_statue,oa/moai,oa/chichen_itza,oa/mayan_step,oa/ufo,"
                      "oa/stone_head,oa/mask,oa/wave,light/point_light,pyramid_ray",
                      "DCC2/Perf",
                      "Comma-separated case-insensitive substrings; performance mode skips "
                      "loading decor models whose data/art/ path matches (default: nature "
                      "decor \"/on/\" and ambient critters \"/ch/\"; obstacles and course "
                      "geometry are never in these categories).");
REXCVAR_DEFINE_BOOL(dcc2_performance_cull_shadows, false, "DCC2/Perf",
                    "Experimental standalone toggle, default off: skip rendering scene "
                    "geometry into the shadow maps (ShadowPass + CubeShadowPass). The maps "
                    "still clear and resolve, but the cleared value reads as shadowed, so "
                    "the whole scene goes dim instead of unshadowed. Not usable until the "
                    "clear value is changed to the lit depth.");
REXCVAR_DEFINE_BOOL(dcc2_performance_cull_water, true, "DCC2/Perf",
                    "Performance mode skips the fullscreen water-surface pass (WaterPass: "
                    "reflection + perturbation composite). The water surface disappears; "
                    "water-related gameplay (fail zones, splashes) is separate and "
                    "unaffected. Set false to keep water while in performance mode.");
REXCVAR_DEFINE_STRING(dcc2_performance_cull_parts,
                      "/on/,/ch/,ab/chunks/mountain,oa/ruin,oa/toltec_statue,oa/moai,"
                      "oa/chichen_itza,oa/head_statue,oa/stone_head,oa/mask,oa/wave,"
                      "oa/ufo,pyramid_ray",
                      "DCC2/Perf",
                      "Comma-separated case-insensitive substrings matched against sub-mesh "
                      "material (texture path) names during model face-data load; matched "
                      "parts register with a zero face count, removing decor baked into course "
                      "chunk meshes (eyes, water planes, bushes) while walkable sub-meshes "
                      "keep drawing. Works below the model-load gate, so it also reaches "
                      "models constructed by the precache bundle path. Default mirrors the "
                      "dcc2_performance_cull categories at part level. Empty = no part culling.");
// LightingPass::DrawPointLights culls each deferred light unless
// (light+184 & g_RenderDevice+72208) != 0. The renderer mask is 0x1 but every light carries
// group 0x2, so without this all point lights are culled and courses render murky.
REXCVAR_DEFINE_INT32(dcc2_light_grp_force, 1, "DCC2",
                     "OR these bits into every registered deferred light's "
                     "group field (light+184) every tick (0 = off). Lights "
                     "carry group 0x2 against renderer mask 0x1, so without "
                     "bit 0 every point light is culled. Workaround: the "
                     "group/mask divergence from the ring path is not "
                     "root-caused.");
REXCVAR_DEFINE_DOUBLE(dcc2_avatar_light_scale, 1.0, "DCC2",
                      "Brightness scale for the forward light set (the lights "
                      "RENDERER::SetupLights feeds avatar/actor draws: the "
                      "menu key light and in-level forward fills). Written "
                      "into each forward light's master-scale field "
                      "(light+472, nominally 1.0) every tick; deferred scene "
                      "lights are untouched. 1.0 = game default; >1 "
                      "brightens avatars (tuning aid for avatars rendering "
                      "dimmer than on Xbox 360).");

// Midasm hooks receive only the registers they name. Helpers below that call recompiled guest
// functions need the full context: the one the hook is running on, owned by the thread state.
static inline PPCContext& GuestCtx() {
  return *rex::runtime::ThreadState::Get()->context();
}
static inline uint8_t* GuestBase() {
  return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0);
}

// True if the guest address is committed, readable guest memory. Uses the runtime's page tables
// (BaseHeap::QueryProtect), not host VirtualQuery: this runs from 40+ call sites per
// SentientUpdate tick on the main thread, where the syscalls would dominate its CPU time.
// Rejects uninitialized or garbage object pointers before they are dereferenced.
static bool DCC2_GuestReadable(uint32_t guest_addr) {
  if (guest_addr < 0x1000u || guest_addr >= 0xC0000000u) {
    return false;
  }
  auto* mem = REX_KERNEL_MEMORY();
  auto* heap = mem->LookupHeap(guest_addr);
  if (!heap) {
    return false;
  }
  uint32_t protect = 0;
  if (!heap->QueryProtect(guest_addr, &protect)) {
    return false;
  }
  return (protect & rex::memory::kMemoryProtectRead) != 0;
}

// Game-services online-state force, the second gate of the LiveGamer content chain. The
// store/items, additional_content and progress refresh only run while the online byte
// *(GUIXA+0x6E30) == 1 (also checked by the connection-lost handler sub_82B01F70). Only the
// Sentient/social connection state machine writes it (stores at 0x82B710FA/0x82B72CF2/
// 0x82B72E3A/0x82B775A6), and it never connects offline, so no HTTP response can set it.
// Forced from the SentientUpdate hook once GUIXA exists: a single-byte write, repeated every
// tick because the state machine writes 0 back.
REXCVAR_DEFINE_BOOL(dcc2_lg_online, true, "DCC2",
                    "Force the game-services online-state flag (*(GUIXA+0x6E30)=1) so the "
                    "LiveGamer store/progress content chain runs against the local emulated "
                    "backend (real costs/unlocks on the native course-select). false = leave "
                    "the game offline (placeholder rows).");
static void DCC2_ForceGameServicesOnline() {
  if (!REXCVAR_GET(dcc2_lg_online)) {
    return;
  }
  auto* mem = REX_KERNEL_MEMORY();
  if (!mem || !DCC2_GuestReadable(kGA_82E73C64)) {
    return;
  }
  const uint32_t guixa =
      __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(kGA_82E73C64));
  if (!guixa || !DCC2_GuestReadable(guixa + 0x6E30u)) {
    return;
  }
  uint8_t* flag = mem->TranslateVirtual<uint8_t*>(guixa + 0x6E30u);
  if (*flag != 1u) {
    *flag = 1u;
  }
}

// Export the native level catalog for the local LiveGamer service, including loaded DLC.
// Profile balances, inventory, unlocks and equipment are owned by the game's HTTP completion
// handlers.
static void DCC2_ExportEconomyCatalog() {
  if (!REXCVAR_GET(dcc2_lg_online)) return;
  auto* mem = REX_KERNEL_MEMORY();
  if (!mem || !DCC2_GuestReadable(kGA_82E73398)) return;
  const auto rd32 = [&](uint32_t address) {
    return __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(address));
  };
  const uint32_t manager = rd32(kGA_82E73398);
  if (!manager || !DCC2_GuestReadable(manager + 12u)) return;
  std::string report = "<lgLevels>\n";
  uint32_t count = 0;
  for (uint32_t record = rd32(manager + 12u);
       record && count < 64u && DCC2_GuestReadable(record + 312u);
       record = rd32(record + 40u), ++count) {
    report += "  <level id=\"" + std::to_string(rd32(record + 128u)) +
              "\" unlock=\"" + std::to_string(rd32(record + 236u)) +
              "\" split=\"" + std::to_string(rd32(record + 240u)) +
              "\" coins=\"" + std::to_string(rd32(record + 220u)) +
              "\" stars=\"" + std::to_string(rd32(record + 224u)) +
              "\" splitCoins=\"" + std::to_string(rd32(record + 228u)) +
              "\" splitStars=\"" + std::to_string(rd32(record + 232u)) + "\"/>\n";
  }
  report += "</lgLevels>\n";
  static std::string previous;
  if (!count || report == previous) return;
  const std::string root = REXCVAR_GET(game_data_root);
  const auto path = std::filesystem::path(root) / "lg_dlc_levels.xml";
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(report.data(), static_cast<std::streamsize>(report.size()));
  file.close();
  if (file) {
    previous = std::move(report);
  }
}

// Shop::GetOrCreate + Refresh (0x82AF4460): creates the Shop singleton (dword_82EB5304;
// sub_82AF2578 builds the 7 categories: Coins, Rewind, powerup, jinx, VanityItem, Courses,
// Featured) and runs Shop::Refresh (0x82AF40F0), which submits the per-category LiveGamer fetch
// task tree to the kernel task system. The Courses task, LiveGamer_ParseLevelItems_BuildTree
// (0x822CA1F0), issues one "store/items?...&p.theme=<theme>" GET per theme: the per-level
// cost/unlock data the course-select renders. The game's own trigger (RefreshShop task
// sub_822D5FD0) waits on the online transition, which never fires offline, so forcing the online
// flag alone does not start the chain.
DECLARE_REX_FUNC(sub_82AF4460);
// KERNELCOMMANDSXA string-assign (0x822409E8): frees the old value and copies the C string into a
// guest-heap string slot. ApplyLgInfoJson uses it for the LG endpoint globals; since it copies, a
// stack-staged source is safe.
DECLARE_REX_FUNC(sub_822409E8);
// Shop store-config-changed (0x82AF4070): Shop::Loading/Reset + destroy the
// Shop singleton so the next GetOrCreate rebuilds it against the new store
// name/id -- ApplyLgInfoJson calls this whenever lgStore name/id change.
DECLARE_REX_FUNC(sub_82AF4070);

// One-shot LG bootstrap and Shop refresh, called from per-frame hooks. The game's LG bootstrap
// (LiveGamer_FetchConfigXml 0x82298B68 -> ApplyLgInfoJson 0x82B013B8) also waits on the online
// transition, so offline the lgServer/lgAuthEndpoint/progressEndpoint/lgStore globals stay null;
// the cert/config.xml fetches at boot belong to the separate Sentient content-update check. This
// seeds those globals host-side through the game's string-assign, then runs store-config-changed
// and Shop refresh; the rest of the chain is game code. The values must agree with
// BuildLgConfigXml in src/kernel/xam/xhttp.cpp (the interceptor matches path segments only).
// ctx belongs to the calling midasm hook and is saved/restored around the guest calls.
static void DCC2_KickShopRefreshOnce() {
  PPCContext& ctx = GuestCtx();
  uint8_t* base = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0);
  static std::atomic<bool> kicked{false};
  static std::atomic<uint32_t> settle{0};
  if (kicked.load(std::memory_order_relaxed) || !REXCVAR_GET(dcc2_lg_online)) {
    return;
  }
  auto* mem = REX_KERNEL_MEMORY();
  if (!mem || !DCC2_GuestReadable(kGA_82E73C64)) {
    return;
  }
  // Signed-in sessions take the game's own online transition, so ApplyLgInfoJson
  // fills lgServer itself and Shop::Refresh has already run. Kicking again here
  // would destroy that Shop while its category fetch tasks are still in flight.
  if (DCC2_GuestReadable(kGA_82EBD2A8) &&
      __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(kGA_82EBD2A8)) != 0u) {
    kicked.store(true, std::memory_order_relaxed);
    REXKRNL_DEBUG("[lgonline] lgServer already set by the game's own LG bootstrap; "
                 "skipping the host-side shop kick");
    return;
  }
  const uint32_t guixa =
      __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(kGA_82E73C64));
  // dword_82EB52DC: manager Shop::Refresh derefs at +128 -- must exist first.
  const uint32_t shopmgr =
      DCC2_GuestReadable(kGA_82EB52DC)
          ? __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(kGA_82EB52DC))
          : 0u;
  // The LEVELCONFIGMANAGER theme list (mgr+92 begin / mgr+96 end) must be populated:
  // ParseLevelItems_BuildTree iterates it for the per-theme store/items fetches, and an empty
  // list issues none.
  uint32_t lvl_begin = 0, lvl_end = 0;
  if (DCC2_GuestReadable(kGA_82E73398)) {
    const uint32_t mgr =
        __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(kGA_82E73398));
    if (mgr && DCC2_GuestReadable(mgr + 92u) && DCC2_GuestReadable(mgr + 96u)) {
      lvl_begin = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(mgr + 92u));
      lvl_end = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(mgr + 96u));
    }
  }
  if (!guixa || !shopmgr || lvl_begin == 0 || lvl_begin == lvl_end) {
    return;
  }
  // The additional-content chain must finish (AdditionalContentXA state 1) before the kick.
  // ParseLevelItems_BuildTree (0x822CA1F0) snapshots the theme list and inserts one course-list
  // object per theme into the Shop Courses map (category+12); the DLC AddPack (dlc_hongkong)
  // registers its theme later. Kicking early builds a 4-theme map, the course-select binds a null
  // course-list for Hong Kong, and std::map lower_bound (0x822CB080) loops forever on the zeroed
  // null page. Gives up waiting after ~60 s so a stuck content chain cannot block the shop.
  bool addcontent_done = false;
  if (DCC2_GuestReadable(kGA_82EB52B4)) {
    const uint32_t ac =
        __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(kGA_82EB52B4));
    if (ac && DCC2_GuestReadable(ac)) {
      addcontent_done = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(ac)) == 1u;
    }
  }
  if (!addcontent_done) {
    static std::atomic<uint32_t> ac_waits{0};
    if (ac_waits.fetch_add(1, std::memory_order_relaxed) < 3600u) {
      return;
    }
    REXKRNL_WARN("[lgonline] shop kick: additional-content chain never reached state 1; "
                 "kicking anyway (DLC themes may be missing from the shop)");
  }
  // Settle ~5s of menu frames after everything exists before the guest calls.
  if (settle.fetch_add(1, std::memory_order_relaxed) < 300u) {
    return;
  }
  kicked.store(true, std::memory_order_relaxed);

  // Stage the endpoint strings in free guest stack below ctx.r1 (sub_822409E8
  // copies them to the guest heap, so the staging is call-scoped only).
  static const struct {
    uint32_t slot;
    const char* value;
  } kLgGlobals[] = {
      {kGA_82EBD2A8, "http://dcc2.service.rexglue/livegamer"},  // lgServer
      {kGA_82EBD2AC, "lg"},                                     // lgAuthEndpoint (path segment)
      {kGA_82EBD2A4, "http://dcc2.service.rexglue/progress"},   // progressEndpoint
      {kGA_82EBD2B0, "CrashCourse2"},                           // lgStore.name
      {kGA_82EBD2B4, "1480659535"},                             // lgStore.id (stored as text)
  };
  uint32_t total = 0;
  for (const auto& g : kLgGlobals) {
    total += static_cast<uint32_t>(std::strlen(g.value)) + 1u;
  }
  const uint32_t block = (ctx.r1.u32 - (total + 0x200u)) & ~0xFu;
  char* host = mem->TranslateVirtual<char*>(block);
  if (!host) {
    REXKRNL_WARN("[lgonline] shop kick: guest stack staging failed");
    return;
  }
  PPCContext saved = ctx;
  ctx.r1.u32 = block - 0x40u;
  uint32_t cursor = 0;
  for (const auto& g : kLgGlobals) {
    const uint32_t len = static_cast<uint32_t>(std::strlen(g.value)) + 1u;
    std::memcpy(host + cursor, g.value, len);
    ctx.r3.u64 = g.slot;
    ctx.r4.u64 = block + cursor;
    sub_822409E8(ctx, base);
    cursor += len;
  }
  // isProduction (byte_82EB52E5) is left unset: it switches the Title Storage profile blob from
  // profile/v1.bin to profile/prod.bin, orphaning existing local saves.
  // Store config changed -> drop any half-built shop, then create + refresh: builds the
  // per-category LiveGamer fetch task tree (store/items per theme).
  sub_82AF4070(ctx, base);
  sub_82AF4460(ctx, base);
  ctx = saved;
  REXKRNL_INFO("[lgonline] LG globals seeded host-side (lgServer/auth/progress/store) + Shop "
               "refresh submitted -- store/items fetches should follow");
}

// registers = ["r3"], return_on_true: overwrite the return value (r3) and skip
// the real SentientUpdate body (0x580004 = Sentient connection established).
bool DCC2_SentientConnected(PPCRegister& r3) {
  // One-shot: set the guest null page's _Isnil byte (offset 145 in the shop/marketplace std::map
  // _Tree_node layout). Guest null reads return zeros, so a freed or corrupt tree walk that
  // reaches node 0 sees _Isnil = 0 and descends null->null forever, pegging a core. With
  // _Isnil(0) = 1, operator[]/find/lower_bound stop at node 0 as at a sentinel; iteration-style
  // walks stop their inner spin, but recovery depends on the caller's loop. Defense in depth: the
  // task-lifetime use-after-free itself is avoided by the xhttp_response_delay_ms latency gate.
  {
    static bool nil_poisoned = false;
    if (!nil_poisoned) {
      nil_poisoned = true;
      auto* mem = REX_KERNEL_MEMORY();
      uint8_t* p = mem->TranslateVirtual<uint8_t*>(145u);
      MEMORY_BASIC_INFORMATION mbi{};
      if (p && VirtualQuery(p, &mbi, sizeof(mbi))) {
        if (mbi.State != MEM_COMMIT) {
          VirtualAlloc(p, 1, MEM_COMMIT, PAGE_READWRITE);
        } else if (!(mbi.Protect &
                     (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
          DWORD old = 0;
          VirtualProtect(p, 1, PAGE_READWRITE, &old);
        }
        MEMORY_BASIC_INFORMATION chk{};
        if (VirtualQuery(p, &chk, sizeof(chk)) && chk.State == MEM_COMMIT &&
            (chk.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
          *p = 1;
        } else {
          REXKRNL_WARN("[nilpoison] guest null page not writable -- skipped");
        }
      }
    }
  }
  // The game-services online force, level-catalog export and shop kick run from this poll. It
  // runs every tick on the main thread; the helpers only need ~10 Hz, so they are rate-limited to
  // keep their cost out of the frame budget.
  {
    using SteadyClock = std::chrono::steady_clock;
    static SteadyClock::time_point last_helper_run{};  // main-thread only
    const auto now = SteadyClock::now();
    if (last_helper_run == SteadyClock::time_point{} ||
        now - last_helper_run >= std::chrono::milliseconds(100)) {
      last_helper_run = now;
      // Light-group fix: OR dcc2_light_grp_force into every registered light's
      // group (light+184, a sticky per-light field read by the DrawPointLights
      // cull test).
      {
        auto* mem = REX_KERNEL_MEMORY();
        auto rd = [&](uint32_t a) -> uint32_t {
          if (a < 0x1000u || a >= 0xC0000000u) return 0u;
          const uint32_t* p = mem->TranslateVirtual<const uint32_t*>(a);
          return p ? __builtin_bswap32(*p) : 0u;
        };
        auto wr = [&](uint32_t a, uint32_t v) {
          if (a < 0x1000u || a >= 0xC0000000u) return;
          uint32_t* p = mem->TranslateVirtual<uint32_t*>(a);
          if (p) *p = __builtin_bswap32(v);
        };
        const uint32_t lmgr = rd(kGA_82E73C5C);
        const uint32_t gforce = uint32_t(REXCVAR_GET(dcc2_light_grp_force));
        if (lmgr && gforce) {
          const uint32_t lbeg = rd(lmgr + 300), lend = rd(lmgr + 304);
          const uint32_t n = (lend - lbeg) / 4;
          if (lbeg && n && n < 4096) {
            for (uint32_t i = 0; i < n; i++) {
              const uint32_t l = rd(lbeg + 4 * i);
              if (l) wr(l + 184, rd(l + 184) | gforce);
            }
          }
        }
        // Avatar brightness: write the cvar into every forward light's
        // master-scale field (light+472; SetupLights multiplies diffuse by
        // (+252)*(+472)*(+176), and +472 is nominally 1.0). Forward lights
        // feed avatar/actor forward draws only; deferred scene lights are
        // a separate list. Absolute write each tick (no compounding);
        // skipped at the 1.0 default.
        const float lscale = float(REXCVAR_GET(dcc2_avatar_light_scale));
        if (lmgr && lscale != 1.0f && lscale > 0.0f && lscale <= 16.0f) {
          const uint32_t fwd_n = rd(lmgr + 180);
          for (uint32_t i = 0; i < fwd_n && i < 16; i++) {
            const uint32_t l = rd(lmgr + 48 + 4 * i);
            if (l) {
              uint32_t bits;
              std::memcpy(&bits, &lscale, 4);
              wr(l + 472, bits);
            }
          }
        }
      }
      DCC2_ForceGameServicesOnline();
      DCC2_ExportEconomyCatalog();
      DCC2_KickShopRefreshOnce();
    }
  }
  r3.u64 = 0x00580004ull;
  return true;
}

// Runtime XMA-decoder kick (src/audio/xma_decoder.cpp). Forward-declared because this project
// builds against the installed SDK headers, which can lag the SDK source tree.
namespace rex::audio {
void KickXmaDecoder();
}

// Kick the XMA decoder at media-stream wait entry. The stream's 0xFFCAx000 "hardware cursors"
// point into the runtime's XMA context array, so the wait is on the XMA decoder worker. The guest
// frees output-ring space with a plain memory write, which the worker only notices on its 1 ms
// re-poll; waking it at wait entry removes that latency. Force-pumping a healthy stream instead
// double-consumes it and stalls the boot intro.
REXCVAR_DEFINE_BOOL(dcc2_media_wait_kick, true, "DCC2",
                    "Wake the XMA decoder worker at media-stream wait entry so decode "
                    "progress resolves in decode-time instead of the worker's 1ms poll "
                    "latency (~4 waits/frame on the main thread in-level).");

// The XMA output ring drains at real-time playback rate (a context re-decodes only after the
// consumer frees ring space), so wait wall time is audio pacing and the kick does not shorten it.
// The CPU is reclaimable: the guest spins the watchdog ~25M iterations/s on the main thread for
// the whole wait. After a grace window of full-speed iterations (covers short waits), each
// iteration sleeps 100 us. The guest's timeout math is wall-clock based, so its decisions are
// unchanged; a wait exits at most ~100 us late.
REXCVAR_DEFINE_BOOL(dcc2_media_wait_sleep, true, "DCC2",
                    "Sleep 100us per media-wait watchdog iteration (after a 1024-iteration "
                    "grace window) instead of busy-spinning -- reclaims the ~60% of the "
                    "main-thread core the in-level audio-pacing waits burn.");

// Reset at every wait entry; counts watchdog iterations within one wait.
static thread_local uint32_t media_spin_iters = 0;

void DCC2_MediaWatchdogSleep(PPCRegister& r3) {
  // Hot path during the grace window: keep the pre-sleep check to a counter and compare;
  // anything heavier shows up in frame times.
  (void)r3;
  if (++media_spin_iters > 1024 && REXCVAR_GET(dcc2_media_wait_sleep)) {
    cc2::thread::SleepPrecise(std::chrono::microseconds(100));
  }
}

// Media-wait entry (TU1 0x82547C20, the spin-wait "advance stream to target"
// function).
void DCC2_MediaWaitKick(PPCRegister& r3, PPCRegister& r4) {
  (void)r3;
  (void)r4;
  // New wait: reset the watchdog spin counter (grace window for the sleep
  // fallback in DCC2_MediaWatchdogSleep).
  media_spin_iters = 0;
  // The guest enters this wait right after freeing output-ring space /
  // staging input with plain guest-memory writes; wake the XMA worker now so
  // the decode progress this wait polls for happens immediately (see the
  // dcc2_media_wait_kick cvar comment).
  if (REXCVAR_GET(dcc2_media_wait_kick)) {
    rex::audio::KickXmaDecoder();
  }
}

// Guest read-fault tolerance. Offline race/render code reads pointers from uninitialized struct
// fields: null ones land in the committed null page (DCC2_CommitGuestNullPage), wild ones can hit
// uncommitted guest pages (e.g. 0x52656771 in the 0x40000000 heap). On a read access violation
// inside the guest mapping, this VEH handler commits the 64 KiB page zero-filled and returns
// handled with RIP unchanged, so the read retries and returns 0. Writes and faults outside the
// guest mapping fall through: write-watch/GPU tracking stays intact and host bugs still crash.
static uint64_t g_guest_membase = 0;

static bool DCC2_GuestReadFaultTolerant(rex::arch::Exception* ex, void* /*data*/) {
  if (ex->code() != rex::arch::Exception::Code::kAccessViolation) {
    return false;
  }
  const uint64_t fault = ex->fault_address();
  if (ex->access_violation_operation() != rex::arch::Exception::AccessViolationOperation::kRead) {
    // Leave writes alone (don't disturb write-watch / GPU tracking), but name
    // the host site first: the runtime's own report carries no PC.
    if (fault >= g_guest_membase && fault < g_guest_membase + 0x200000000ull) {
      const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
      REXKRNL_ERROR("[faultdiag] WRITE @guest 0x{:08X} from host pc 0x{:016X} (exe+0x{:X})",
                    static_cast<uint32_t>(fault - g_guest_membase), ex->pc(),
                    ex->pc() >= base ? ex->pc() - base : 0);
    }
    return false;
  }
  // Guest mapping = [membase, membase + 8 GiB): 4 GiB virtual + 4 GiB physical
  // mirror. Only tolerate faults inside it.
  if (fault < g_guest_membase || fault >= g_guest_membase + 0x200000000ull) {
    return false;
  }
  void* page = reinterpret_cast<void*>(fault & ~uint64_t(0xFFFFu));
  DWORD old = 0;
  if (VirtualProtect(page, 0x10000u, PAGE_READWRITE, &old) ||
      VirtualAlloc(page, 0x10000u, MEM_COMMIT, PAGE_READWRITE)) {
    return true;  // page now readable (zero) -> retry the faulting read
  }
  return false;  // couldn't map it; let it crash rather than spin
}

// Commits guest [0, 64 KiB) as zero-filled read/write memory and installs the read-fault handler.
// Offline code walks structures with unbuilt sub-objects and reads null + small offsets; those
// reads return 0 and pointer chains through null stay in the page. Calls through a null target
// are still caught by the invalid-function trap. The guest view maps the null page PAGE_NOACCESS,
// hence VirtualProtect. One-time.
// Non-static: Cc2App::OnPreLaunchModule (cc2_app.h) calls it before the first guest thread runs.
// The Kernel Dispatch thread can enter a D3D ring-write helper (TU 0x82549F40 = base 0x8253A870)
// with r3 = NULL before the device global is set; its reads return 0 and its packet writes land
// in the zeroed null region.
void DCC2_CommitGuestNullPage() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  // Cache the guest mapping base and install the read-fault tolerance handler.
  g_guest_membase =
      reinterpret_cast<uint64_t>(REX_KERNEL_MEMORY()->TranslateVirtual<void*>(0u));
  rex::arch::ExceptionHandler::Install(&DCC2_GuestReadFaultTolerant, nullptr);
  // Invalid-call tolerance is unconditional (src/kernel/function_dispatch.cpp).
  auto* mem = REX_KERNEL_MEMORY();
  void* host = mem->TranslateVirtual<void*>(0u);  // host address of guest 0
  constexpr SIZE_T kSize = 0x10000;               // 64 KiB null region
  DWORD old = 0;
  if (VirtualProtect(host, kSize, PAGE_READWRITE, &old)) {
    std::memset(host, 0, kSize);
  } else if (VirtualAlloc(host, kSize, MEM_COMMIT, PAGE_READWRITE)) {
    std::memset(host, 0, kSize);
  } else {
    REXKRNL_WARN("[racefix] failed to map guest null region (err {})", (uint32_t)GetLastError());
  }
}

// MIDASM @ the "menus" scene-composition stage Render vmethod (base sub_82AED2D8), every frame on
// the guest thread: the per-frame UI ticks and the one-shot shop kick. registers = [].
void DCC2_SceneComposeTick() {
  PPCContext& ctx = GuestCtx();
  uint8_t* base = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0);
  cc2::display::Tick(ctx, base);
  cc2::lobby::Tick(ctx, base);
  cc2::fame::LobbyTick(ctx, base);
  cc2::mouse::Tick(ctx, base);
  cc2::mouse::GloveTick(ctx, base);
  cc2::kernel::xam::PollLocalPlayers();
  // Backup trigger for the shop kick: SentientUpdate stops polling before the LG bootstrap parses.
  DCC2_KickShopRefreshOnce();
}

// Per-race AI ghost avatar re-roll. World Tour/Time Trial ghost avatars come from
// AVATARMODELMANAGER's random-metadata pool (RetrieveMetadata 0x829DD0B0 type 2: mgr+172 vector,
// filled once per session via XamAvatarGetMetadataRandom), and each pool AVATARMODELMASTER keeps
// its built buffers for the session, so ghosts look the same every race. At race setup entry
// (sub_82B60008, gameclip/ghost construction) this applies the OnPlayerAvatarChanged (0x829DE170)
// recipe to the random-pool masters: for each master in the manager's list (mgr+12, next at +40)
// with type (+744) == 2, idle (+32 != 1) and referenced (+24 > 0), set the metadata-dirty byte
// (+1168) and call StartDataLoading (sub_829DD768). With metadata present that re-runs
// BuildAvatarBuffers -> XamAvatarGetAssets, which draws a new look from the runtime's AI-look deck.
DECLARE_REX_FUNC(sub_829DD768);  // AVATARMODELMASTER::StartDataLoading
REXCVAR_DEFINE_BOOL(dcc2_ai_reroll_per_race, true, "DCC2",
                    "Re-roll the AI ghost avatars at every race setup so each run gets a "
                    "fresh random cast (the game builds them once per session otherwise). "
                    "Requires dcc2_ai_avatar_randomize.");
void DCC2_RerollGhostAvatars(PPCRegister& r3, PPCRegister& r4) {
  PPCContext& ctx = GuestCtx();
  uint8_t* base = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0);
  if (!REXCVAR_GET(dcc2_ai_reroll_per_race)) {
    return;
  }
  auto* mem = REX_KERNEL_MEMORY();
  if (!mem || !DCC2_GuestReadable(kGA_82EB5288)) {
    return;
  }
  const auto rd32 = [&](uint32_t ga) {
    return __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(ga));
  };
  const uint32_t mgr = rd32(kGA_82EB5288);
  if (!mgr || !DCC2_GuestReadable(mgr + 12u)) {
    return;
  }
  uint32_t master = rd32(mgr + 12u);
  for (uint32_t guard = 0; master && guard < 64u && DCC2_GuestReadable(master + 1168u); ++guard) {
    const uint32_t next = rd32(master + 40u);
    if (rd32(master + 744u) == 2u /* random-pool metadata */ &&
        rd32(master + 32u) != 1u /* not mid-build */ &&
        static_cast<int32_t>(rd32(master + 24u)) > 0 /* referenced */) {
      *mem->TranslateVirtual<uint8_t*>(master + 1168u) = 1;  // metadata dirty
      PPCContext saved_ctx = ctx;
      r3.u64 = master;
      r4.u64 = 0;
      sub_829DD768(ctx, base);
      ctx = saved_ctx;
    }
    master = next;
  }
}

// Time Trial (Nemesis) default game clips. GAMEXA::UpdateLoading runs the local clip loader
// sub_82B5B558 (data/clips/<level>/*.clip: three shipped, time-graded ghost runs per level, named
// <M_SS_mmm>_<vanity>.clip; records at unk_82EBB258) only when game mode (g_GameModeMgr+0x100)
// == 0. Time Trial ghosts normally come from the RT "add a game clip" lobby, which queries the
// leaderboard/friends backend and is always empty offline. Hooked on the mode-check bne
// (0x82B62E08): returning true jumps to the fall-through (0x82B62E0C), so Time Trial loads the
// same three ghosts as World Tour.
REXCVAR_DEFINE_BOOL(dcc2_tt_default_clips, true, "DCC2",
                    "Load the level's shipped default game clips (data/clips/<level>/) in "
                    "Time Trial (mode 1) as well as World Tour, so offline Time Trials have ghost opponents despite "
                    "the dead leaderboard 'add a game clip' lobby. false = native gating "
                    "(local clips in mode 0 / World Tour only).");
bool DCC2_ForceLocalClipsInNemesis(PPCRegister& r3) {
  (void)r3;
  if (!REXCVAR_GET(dcc2_tt_default_clips)) {
    return false;  // native behavior: mode != 0 skips the local clip loader
  }
  uint32_t mode = 0xFFFFFFFFu;
  auto* mem = REX_KERNEL_MEMORY();
  if (mem && DCC2_GuestReadable(kGA_82EB52B8)) {
    const uint32_t mgr = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(kGA_82EB52B8));
    if (mgr && DCC2_GuestReadable(mgr + 0x100u)) {
      mode = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(mgr + 0x100u));
    }
  }
  // Time Trial (mode 1) only: sub_82B7C550 ranks players+ghosts in a 4-slot stack buffer, so
  // three ghosts on top of an online lobby smash its /GS cookie (0xC0000409 -> KeBugCheck).
  if (mode != 1u) {
    return false;
  }
  return true;  // jump to the sub_82B5B558 fall-through
}

// Time Trial A-press gate. NEMESISLAUNCHER::Launch (0x82AD52D0) refuses to launch when its
// downloaded-clip vector [obj+0x50, obj+0x54) is empty (unistring not_ready_for_ttrials, shown by
// the Lua A-handler when LaunchNemesis returns false). Offline the vector stays empty: entries
// come from a leaderboard query + GameclipDownloadTask and wrap live peer-profile objects that
// cannot be fabricated safely. Every consumer inside Launch is empty-safe, so returning true jumps
// to the fall-through (0x82AD5308) and the native launch path (task tree, mode transition) runs
// with zero lobby clips. The busy (+0x60) and pending-download (+0x48) gates still apply.
// In-race ghosts come from dcc2_tt_default_clips.
REXCVAR_DEFINE_BOOL(dcc2_tt_launch_without_clips, true, "DCC2",
                    "Let Time Trials launch with an empty game-clip lobby (offline the RT "
                    "add-a-game-clip leaderboard is dead, so the lobby can never be "
                    "filled). Ghost opponents still come from dcc2_tt_default_clips.");
bool DCC2_AllowEmptyNemesisLaunch(PPCRegister& r3) {
  (void)r3;
  if (!REXCVAR_GET(dcc2_tt_launch_without_clips)) {
    return false;  // native rule: no clips -> no launch (not_ready_for_ttrials)
  }
  return true;
}

// sub_82B5B3B0 attaches a racer's RVMP model to its render actor (a1 = r3 = the racer's
// field_0). Offline the render actors are never constructed, so field_0 is garbage (e.g.
// 0x43fa0000, uncommitted) and *a1 faults or yields an invalid call target. Skips the function
// when a1 is not a constructed object; the caller (sub_82B60008 racer arming) ignores the return
// value. registers = ["r3"], return_on_true = true.
bool DCC2_SkipRacerModelIfInvalid(PPCRegister& r3) {
  const uint32_t a1 = r3.u32;
  bool bad = !DCC2_GuestReadable(a1);
  if (!bad) {
    // *a1 is the C++ vtable pointer. A real object's vtable lives in read-only
    // data; if it lands inside the executable code range (0x82230000..kGA_82CDD740)
    // or isn't readable, a1 is not a constructed object (offline the player's
    // avatar render actor is a placeholder whose first word points at code).
    auto* mem = REX_KERNEL_MEMORY();
    const uint32_t vtable = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(a1));
    bad = !DCC2_GuestReadable(vtable) || (vtable >= 0x82230000u && vtable < kGA_82CDD740);
  }
  return bad;  // valid -> attach normally; unconstructed/placeholder -> skip
}

// The four sub_8228572x thunks each fire the "ContentUpToDateFail" GUI event
// (sub_822447D0) and return 2. Skipping the body suppresses the event so the
// GUI never sets `contentFailed` (no error splash).
#define DCC2_NO_CONTENT_FAIL(n)                          \
  bool DCC2_NoContentFail##n(PPCRegister& r3) {          \
    r3.u64 = 2;                                          \
    return true;                                         \
  }
DCC2_NO_CONTENT_FAIL(1)
DCC2_NO_CONTENT_FAIL(2)
DCC2_NO_CONTENT_FAIL(3)
DCC2_NO_CONTENT_FAIL(4)

// PLAYERPROFILEXENON gamer-picture award pump (base sub_82AE49C8 / TU1 sub_82AF2A98). Submits
// queued awards via XamWriteGamerTile and expects X_ERROR_IO_PENDING; any other result shows
// CANT_SAVE_GAMER_PICT (common/ui/sys/msg/error/save_gamer_picture). Offline the pending count
// goes non-zero while the item block-deque stays in its ctor state (sentinel pointing back at the
// head field, block slots 0), so the pump reads item pointers from the null page,
// XamWriteGamerTile gets overlapped = 0 and returns 0 instead of 997, and the popup fires. In that
// state, discard the phantom entries (count, cursor and in-progress byte zeroed, as the game's
// drained-queue epilogue does) and skip the pump. A populated queue runs natively.
// registers = ["r3"], return_on_true = true.
bool DCC2_SkipPhantomGamerPicQueue(PPCRegister& r3) {
  // PLAYERPROFILEXENON layout: TU1 inserts 16 bytes before the award-queue region.
#ifdef DCC2_TU1
  constexpr uint32_t kOffQueueHead = 5664u, kOffCursor = 5676u, kOffCount = 5680u,
                     kOffBusyByte = 4552u;
#else
  constexpr uint32_t kOffQueueHead = 5648u, kOffCursor = 5660u, kOffCount = 5664u,
                     kOffBusyByte = 4536u;
#endif
  const uint32_t self = r3.u32;
  if (!self) {
    return false;
  }
  auto* mem = REX_KERNEL_MEMORY();
  const auto load_u32 = [&](uint32_t ga) {
    return __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(ga));
  };
  const uint32_t pending = load_u32(self + kOffCount);
  if (!pending) {
    return false;  // nothing queued; native pump early-outs
  }
  // DCC2 awards only two gamer pictures; a large pending count is corrupt state.
  bool phantom = pending > 8u;
  const uint32_t sentinel = load_u32(self + kOffQueueHead);
  const uint32_t first_block = sentinel ? load_u32(sentinel) : 0u;
  if (!phantom) {
    // Empty block list: the sentinel's next pointer still points back at the
    // head field itself (ctor state), so item fetches would hit the null page.
    phantom = !sentinel || !first_block || first_block == self + kOffQueueHead;
  }
  if (!phantom) {
    // List non-empty: also guard the current item pointer (block data array
    // null, or the entry null / low / unaligned -> garbage, not a heap item).
    const uint32_t data_array = load_u32(first_block + 4u);
    uint32_t cursor = load_u32(self + kOffCursor);
    const uint32_t block_base = load_u32(first_block + 8u);
    if (block_base <= cursor) {
      cursor -= block_base;
    }
    const uint32_t item = data_array ? load_u32(data_array + 4u * cursor) : 0u;
    phantom = !data_array || item < 0x10000u || (item & 3u) != 0u;
  }
  if (!phantom) {
    return false;  // real queued award; let the game submit it
  }
  *mem->TranslateVirtual<uint32_t*>(self + kOffCount) = 0;   // pending count
  *mem->TranslateVirtual<uint32_t*>(self + kOffCursor) = 0;  // read cursor
  *mem->TranslateVirtual<uint8_t*>(self + kOffBusyByte) = 0;  // save-in-progress flag
  return true;  // skip the pump for this profile this frame
}

// Performance mode (dcc2_minimal_render): scenery matching dcc2_performance_cull never activates
// or loads, so it costs no updates or draws; gameplay actors run natively.
namespace {

// Actor name: char* at +284 (SCENEXA::DumpActors reads the same field).
void SceneActorName(uint32_t actor, char (&name)[64]) {
  std::snprintf(name, sizeof(name), "<noname>");
  auto* mem = REX_KERNEL_MEMORY();
  const uint32_t name_ga =
      __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(actor + 284u));
  if (name_ga < 0x10000u) {
    return;
  }
  const char* src = mem->TranslateVirtual<const char*>(name_ga);
  size_t i = 0;
  for (; i < sizeof(name) - 1 && src[i]; ++i) {
    name[i] = src[i];
    if (static_cast<unsigned char>(src[i]) < 0x20u ||
        static_cast<unsigned char>(src[i]) > 0x7Eu) {
      break;  // non-printable: not a real name string
    }
  }
  name[i] = '\0';
}

// dcc2_performance_cull, parsed once (cvars are boot-time config here).
// Case-insensitive substring match (model paths like "dlc_jungle/on/palm_01"
// are matched by category fragments like "/on/").
bool SceneActorCulled(const char* name) {
  static const std::vector<std::string> cull = [] {
    std::vector<std::string> out;
    std::string list = REXCVAR_GET(dcc2_performance_cull);
    size_t pos = 0;
    while (pos < list.size()) {
      size_t comma = list.find(',', pos);
      if (comma == std::string::npos) comma = list.size();
      std::string entry = list.substr(pos, comma - pos);
      while (!entry.empty() && entry.front() == ' ') entry.erase(entry.begin());
      while (!entry.empty() && entry.back() == ' ') entry.pop_back();
      std::transform(entry.begin(), entry.end(), entry.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (!entry.empty()) out.push_back(std::move(entry));
      pos = comma + 1;
    }
    return out;
  }();
  if (cull.empty()) {
    return false;
  }
  std::string lower(name);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
  });
  for (const std::string& pat : cull) {
    if (lower.find(pat) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

// ACTOR::SetActive entry (base sub_82A852B8 / TU1 sub_82A92768). r3 = actor,
// r4 = activate flag. Activating inserts the actor into the scene's active layer
// (the per-frame update list) and fires OnActivate; a flag equal to the current
// state (+428) is a no-op, so forcing r4 to 0 skips the activate lifecycle. The
// scene loader's bulk path bypasses SetActive, so DCC2_SceneLayerGate below is
// the authoritative cull; this one keeps SetActive-path actors from running
// OnActivate.
void DCC2_SceneActorGate(PPCRegister& r3, PPCRegister& r4) {
  if (!REXCVAR_GET(dcc2_minimal_render) || !r3.u32 || !r4.u32) {
    return;
  }
  char name[64];
  SceneActorName(r3.u32, name);
  if (SceneActorCulled(name)) {
    r4.u64 = 0;  // never activates: SetActive early-outs on flag==current
  }
}

// SCENE::PutActorOnActiveLayer entry (base sub_82AB8400 / TU1 sub_82AC7780).
// r3 = actor. Every actor passes through here to enter the scene's active
// layer (the per-frame update list) -- both via ACTOR::SetActive and via the
// scene loader's direct vtable call -- so performance mode declines the insert
// here by returning -1 (the function's own "layer full" failure value, which
// callers already tolerate).
bool DCC2_SceneLayerGate(PPCRegister& r3) {
  if (!REXCVAR_GET(dcc2_minimal_render) || !r3.u32) {
    return false;
  }
  char name[64];
  SceneActorName(r3.u32, name);
  if (SceneActorCulled(name)) {
    r3.s64 = -1;  // callers see the layer-full failure value
    return true;  // skip the native insert
  }
  return false;
}

namespace {

// Parsed dcc2_performance_cull_fx (used by the emitter update gate).
const std::vector<std::string>& FxCullList() {
  static const std::vector<std::string> fx_cull = [] {
    std::vector<std::string> out;
    std::string list = REXCVAR_GET(dcc2_performance_cull_fx);
    size_t pos = 0;
    while (pos < list.size()) {
      size_t comma = list.find(',', pos);
      if (comma == std::string::npos) comma = list.size();
      std::string entry = list.substr(pos, comma - pos);
      while (!entry.empty() && entry.front() == ' ') entry.erase(entry.begin());
      while (!entry.empty() && entry.back() == ' ') entry.pop_back();
      std::transform(entry.begin(), entry.end(), entry.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (!entry.empty()) out.push_back(std::move(entry));
      pos = comma + 1;
    }
    return out;
  }();
  return fx_cull;
}

bool FxNameMatches(const char* name) {
  std::string lower(name);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
  });
  for (const std::string& pat : FxCullList()) {
    if (lower.find(pat) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// Level-load generation counter (bumped by DCC2_SkipStaticScene) so cached
// per-object emitter verdicts don't survive guest heap reuse across levels.
std::atomic<uint32_t> g_perf_level_generation{0};

}  // namespace

// StaticGeometry::Load entry (base 0x82A81908 / TU1 0x82A8EDB8). Loads the
// baked distant-backdrop world: data/maps/<level>.be.ssg geometry + the
// art/staticscene .wxm texture atlas. Performance mode skips the entire load;
// the engine's own missing-.ssg path (shipped, error-tolerant: loaded flag
// this+8 stays 0, renderer draws no static scene) covers everything
// downstream. Returns false to the game like a failed load would.
bool DCC2_SkipStaticScene(PPCRegister& r3) {
  if (!REXCVAR_GET(dcc2_minimal_render)) {
    return false;  // run the native load
  }
  // Fires once per level load: bump the generation so the emitter update
  // gate's per-object verdict cache can't go stale across guest heap reuse.
  g_perf_level_generation.fetch_add(1, std::memory_order_relaxed);
  r3.u64 = 0;   // BOOL return: not loaded
  return true;  // skip the native function
}

// RenderVertexModelMASTER::LoadModel entry (base 0x82A33500 / TU1
// 0x82A420D0). r3 = master object, r4 = model path under data/art/ (e.g.
// "dlc_jungle/on/palm_01/..."). Performance mode declines loads matching
// dcc2_performance_cull by returning 0, the function's own shipped failure
// value.
bool DCC2_ModelLoadGate(PPCRegister& r3, PPCRegister& r4) {
  if (!REXCVAR_GET(dcc2_minimal_render) || !r4.u32) {
    return false;
  }
  auto* mem = REX_KERNEL_MEMORY();
  const char* src = mem->TranslateVirtual<const char*>(r4.u32);
  char path[192];
  size_t i = 0;
  for (; i < sizeof(path) - 1 && src[i] &&
         static_cast<unsigned char>(src[i]) >= 0x20u &&
         static_cast<unsigned char>(src[i]) <= 0x7Eu;
       ++i) {
    path[i] = src[i];
  }
  path[i] = '\0';
  if (SceneActorCulled(path)) {
    r3.u64 = 0;   // LoadModel's failure return
    return true;  // skip the native load
  }
  return false;
}

// CAMERA::UpdateMatrices entry (base 0x82AACA58 / TU1 0x82AB9990). r3 = the
// camera object. Layout: mode @+1392 (0 = perspective,
// 1 = ortho, 2 = shadow), far @+1396, near @+1400, proj-dirty flag @+1517.
// Render distance: clamp the perspective far plane; the function's own dirty
// handling rebuilds the projection with the game's math, and the GPU clips
// everything beyond. Reads the cvar every call, so the F4 menu tunes it live.
void DCC2_RenderDistanceGate(PPCRegister& r3) {
  const double limit = REXCVAR_GET(dcc2_render_distance);
  if (limit <= 0.0 || !r3.u32) {
    return;
  }
  auto* mem = REX_KERNEL_MEMORY();
  const uint32_t cam = r3.u32;
  const uint32_t mode = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(cam + 1392u));
  if (mode != 0u) {
    return;  // perspective cameras only: never clamp shadow/ortho cameras
  }
  uint32_t far_be = *mem->TranslateVirtual<const uint32_t*>(cam + 1396u);
  uint32_t far_le = __builtin_bswap32(far_be);
  float far_val;
  std::memcpy(&far_val, &far_le, sizeof(far_val));
  const float clamped = static_cast<float>(limit);
  if (!(far_val > clamped)) {
    return;  // already at or below the limit (also rejects NaN/garbage)
  }
  uint32_t new_le;
  std::memcpy(&new_le, &clamped, sizeof(new_le));
  *mem->TranslateVirtual<uint32_t*>(cam + 1396u) = __builtin_bswap32(new_le);
  *mem->TranslateVirtual<uint8_t*>(cam + 1517u) = 1;  // proj dirty: rebuild
}

// Shadow draw-distance hook. Fires inside CAMERA__UpdateMatrices_ShadowCam
// (base 0x82AABD1C / TU1 0x82AB8C54) right after the SCENEXA::GetShadowBounds
// (vtbl+164, base 0x82BAD708) results are finalized on the stack:
//   [r1+0x70/74/78] half-extents (halved; sets the caster depth range)
//   [r1+0x80/84/88] box center (player/camera + per-level offset)
//   [r1+0x8C]       farSlice: NDC depth on the ref camera (0.995 in-level)
//   [r1+0x90]       ref camera guest ptr (far @+1396, near @+1400)
// Lateral coverage is the ref camera frustum truncated at farSlice. The slice is
// solved from d = f*n/(f - s*(f-n)) for dcc2_shadow_distance and only raised;
// half-extents are floored to dcc2_shadow_min_ext_*. Cvars are read every call.
void DCC2_ShadowBoxFloor(PPCRegister& r1) {
  const double fx = REXCVAR_GET(dcc2_shadow_min_ext_x);
  const double fy = REXCVAR_GET(dcc2_shadow_min_ext_y);
  const double fz = REXCVAR_GET(dcc2_shadow_min_ext_z);
  const double dist = REXCVAR_GET(dcc2_shadow_distance);
  if (fx <= 0.0 && fy <= 0.0 && fz <= 0.0 && dist <= 0.0) {
    return;
  }
  auto* mem = REX_KERNEL_MEMORY();
  const uint32_t sp = r1.u32;
  auto rdf = [&](uint32_t a) {
    uint32_t le = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(a));
    float f;
    std::memcpy(&f, &le, sizeof(float));
    return f;
  };
  auto wrf = [&](uint32_t a, float f) {
    uint32_t le;
    std::memcpy(&le, &f, sizeof(float));
    *mem->TranslateVirtual<uint32_t*>(a) = __builtin_bswap32(le);
  };
  const uint32_t off[3] = {0x70u, 0x74u, 0x78u};  // half-extents X/Y/Z
  const double flr[3] = {fx, fy, fz};
  float cur[3];
  for (int i = 0; i < 3; ++i) {
    cur[i] = rdf(sp + off[i]);
  }
  // Ref camera near/far -> what world distance does the current slice reach?
  const float slice = rdf(sp + 0x8Cu);
  const uint32_t cam = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(sp + 0x90u));
  float camFar = 0.0f, camNear = 0.0f;
  if (cam) {
    camFar = rdf(cam + 1396u);
    camNear = rdf(cam + 1400u);
  }
  const bool camOk = cam && camNear > 0.0f && camFar > camNear;
  // Raise the NDC slice so coverage reaches dcc2_shadow_distance.
  if (dist > 0.0 && camOk) {
    double d = dist;
    const double fmax = static_cast<double>(camFar) * 0.999;
    if (d > fmax) {
      d = fmax;  // can't cover beyond the camera's own far plane
    }
    if (d > camNear) {
      const double sNew =
          (static_cast<double>(camFar) * (d - camNear)) / (d * (static_cast<double>(camFar) - camNear));
      if (sNew > slice && sNew < 1.0) {
        wrf(sp + 0x8Cu, static_cast<float>(sNew));  // extend only, never shrink
      }
    }
  }
  for (int i = 0; i < 3; ++i) {
    if (flr[i] <= 0.0) {
      continue;
    }
    const float f = static_cast<float>(flr[i]);
    if (cur[i] >= f) {
      continue;  // already at or above the floor: leave untouched
    }
    wrf(sp + off[i], f);
  }
}

// Mesh3::LoadModel entry (base 0x82BC45B8 / TU1 0x82BD9F30). CC2's own
// world-component (WCOM) mesh system -- course chunks and .wbm-placed world
// decor (bushes, trees, water edges, eyes, ray shafts) load here, not through
// RenderVertexModelMASTER. r3 = mesh path under data/art/, r4 = Mesh3 out
// struct, r5 = WCOM instance name. Declining leaves a meshless WCOM (every
// failure path in the shipped function does the same); collision is a
// separate system so gameplay physics is unaffected.
bool DCC2_WcomMeshGate(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  if (!REXCVAR_GET(dcc2_minimal_render) || !r3.u32) {
    return false;
  }
  (void)r4;
  auto* mem = REX_KERNEL_MEMORY();
  const char* src = mem->TranslateVirtual<const char*>(r3.u32);
  char path[192];
  size_t i = 0;
  for (; i < sizeof(path) - 1 && src[i] &&
         static_cast<unsigned char>(src[i]) >= 0x20u &&
         static_cast<unsigned char>(src[i]) <= 0x7Eu;
       ++i) {
    path[i] = src[i];
  }
  path[i] = '\0';
  char wcom[96] = "<unnamed>";
  if (r5.u32 >= 0x10000u) {
    const char* ws = mem->TranslateVirtual<const char*>(r5.u32);
    size_t w = 0;
    for (; w < sizeof(wcom) - 1 && ws[w] && static_cast<unsigned char>(ws[w]) >= 0x20u &&
           static_cast<unsigned char>(ws[w]) <= 0x7Eu;
         ++w) {
      wcom[w] = ws[w];
    }
    wcom[w] = '\0';
  }
  // Cull on either the mesh path or the WCOM instance name (both carry the
  // decor category, e.g. "dlc_jungle/on/bush_01/...").
  if (SceneActorCulled(path) || SceneActorCulled(wcom)) {
    r3.u64 = 0;   // LoadModel's failure return
    return true;  // skip: WCOM stays meshless
  }
  return false;
}

// ANIMATEDMODELMASTER::_ProcessConfigFile entry (base 0x82A5AB38 / TU1
// 0x82A67F20). r4 = animated-model cfg name. Animated scenery (crocodile,
// UFO, lilypads) loads through this master, not RenderVertexModelMASTER.
// Performance mode declines cull-matched loads (callers already handle the
// function's own empty-cfg-name failure path).
bool DCC2_AnimModelGate(PPCRegister& r3, PPCRegister& r4) {
  if (!REXCVAR_GET(dcc2_minimal_render) || !r4.u32) {
    return false;
  }
  auto* mem = REX_KERNEL_MEMORY();
  const char* src = mem->TranslateVirtual<const char*>(r4.u32);
  char path[192];
  size_t i = 0;
  for (; i < sizeof(path) - 1 && src[i] &&
         static_cast<unsigned char>(src[i]) >= 0x20u &&
         static_cast<unsigned char>(src[i]) <= 0x7Eu;
       ++i) {
    path[i] = src[i];
  }
  path[i] = '\0';
  if (SceneActorCulled(path)) {
    r3.u64 = 0;   // the error paths' return value
    return true;  // skip the native cfg processing
  }
  return false;
}

// PARTICLEEMITTERMASTER update/simulate entry (base 0x82A23B08 / TU1
// 0x82A326D8; r3 = emitter, f1 = dt). Skipping the update means the emitter
// exists (loader is satisfied) but never spawns, simulates, or draws a
// particle. The emitter's instance-name storage offset is unknown, so each
// object is classified once by scanning its first 1 KB for the name --
// inline text or a pointed-to string -- against dcc2_performance_cull_fx;
// verdicts are cached per object and invalidated on level load (static-scene
// skip bumps the generation) to survive guest heap reuse.
bool DCC2_EmitterUpdateGate(PPCRegister& r3) {
  if (!REXCVAR_GET(dcc2_minimal_render) || FxCullList().empty() || !r3.u32) {
    return false;
  }
  const uint32_t obj = r3.u32;
  static std::mutex verdict_mutex;
  static std::map<uint32_t, bool> verdicts;
  static uint32_t verdict_generation = 0;
  std::lock_guard<std::mutex> lock(verdict_mutex);
  const uint32_t gen = g_perf_level_generation.load(std::memory_order_relaxed);
  if (gen != verdict_generation) {
    verdicts.clear();
    verdict_generation = gen;
  }
  auto it = verdicts.find(obj);
  if (it == verdicts.end()) {
    auto* mem = REX_KERNEL_MEMORY();
    bool cull = false;
    char text[97];
    // Pass 1: inline name text anywhere in the object's first 1 KB.
    const char* raw = mem->TranslateVirtual<const char*>(obj);
    for (uint32_t off = 0; off + 4u <= 1024u && !cull; off += 4u) {
      size_t n = 0;
      for (; n < sizeof(text) - 1 && raw[off + n] &&
             static_cast<unsigned char>(raw[off + n]) >= 0x20u &&
             static_cast<unsigned char>(raw[off + n]) <= 0x7Eu && off + n < 1023u;
           ++n) {
        text[n] = raw[off + n];
      }
      text[n] = '\0';
      if (n >= 4u && FxNameMatches(text)) {
        cull = true;
      }
    }
    // Pass 2: pointer-chased strings (name held via a heap pointer).
    for (uint32_t off = 0; off + 4u <= 1024u && !cull; off += 4u) {
      const uint32_t ptr =
          __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(obj + off));
      if (ptr < 0x10000u || ptr >= 0xC0000000u) {
        continue;
      }
      const char* s = mem->TranslateVirtual<const char*>(ptr);
      size_t n = 0;
      for (; n < sizeof(text) - 1 && s[n] && static_cast<unsigned char>(s[n]) >= 0x20u &&
             static_cast<unsigned char>(s[n]) <= 0x7Eu;
           ++n) {
        text[n] = s[n];
      }
      text[n] = '\0';
      if (n >= 4u && FxNameMatches(text)) {
        cull = true;
      }
    }
    it = verdicts.emplace(obj, cull).first;
  }
  if (it->second) {
    r3.u64 = 0;
    return true;  // skip the simulation update
  }
  return false;
}

namespace {

// Parsed dcc2_performance_cull_parts (sub-mesh material/texture-name culling).
const std::vector<std::string>& PartsCullList() {
  static const std::vector<std::string> parts_cull = [] {
    std::vector<std::string> out;
    std::string list = REXCVAR_GET(dcc2_performance_cull_parts);
    size_t pos = 0;
    while (pos < list.size()) {
      size_t comma = list.find(',', pos);
      if (comma == std::string::npos) comma = list.size();
      std::string entry = list.substr(pos, comma - pos);
      while (!entry.empty() && entry.front() == ' ') entry.erase(entry.begin());
      while (!entry.empty() && entry.back() == ' ') entry.pop_back();
      std::transform(entry.begin(), entry.end(), entry.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (!entry.empty()) out.push_back(std::move(entry));
      pos = comma + 1;
    }
    return out;
  }();
  return parts_cull;
}

bool PartNameMatches(const char* name) {
  std::string lower(name);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
  });
  for (const std::string& pat : PartsCullList()) {
    if (lower.find(pat) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// Sub-mesh (part) culling by texture name. The .rvm material table @master+300
// (8 bytes/part) holds a 64-bit renderer ID pair, not a pointer (e.g. 0x9BA).
// sub_82A30600's material loop derives the ID from the texture path
// (LoadMaterialNames record -> TEXTUREMASTER lookup/create -> renderer
// vtbl+0x90 -> "stdx r8,r29,r9" table store). The capture hook below runs at
// that store with the name and the ID in registers; matched IDs go in a set,
// and the LoadFacesData registration sites zero the face count of any part
// whose table entry is in the set. Generation-invalidated like the emitter
// verdicts (renderer IDs can be reused across level loads).
std::mutex g_part_ids_mutex;
std::set<uint64_t> g_culled_part_ids;
uint32_t g_part_ids_generation = 0;

// The table-init filler {-1 dword, -1 word, 0 word} and a null pair must
// never enter the cull set: unmatched parts share those values.
bool PartIdDegenerate(uint64_t id) {
  return id == 0u || id == 0xFFFFFFFFFFFF0000ull;
}

// Reads the part's material ID from guest memory at the LoadFacesData sites:
// the master object is spilled at *(r1+0x174) by the prologue, table
// @master+300, 8 bytes per part.
bool PartIdCulled(uint32_t sp, uint32_t part_idx) {
  if (!REXCVAR_GET(dcc2_minimal_render) || PartsCullList().empty() || !sp) {
    return false;
  }
  auto* mem = REX_KERNEL_MEMORY();
  const auto load_u32 = [&](uint32_t ga) {
    return __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(ga));
  };
  const uint32_t master = load_u32(sp + 0x174u);
  if (master < 0x10000u || master >= 0xC0000000u) {
    return false;
  }
  const uint32_t table = load_u32(master + 300u);
  if (table < 0x10000u || table >= 0xC0000000u) {
    return false;
  }
  const uint64_t id =
      __builtin_bswap64(*mem->TranslateVirtual<const uint64_t*>(table + part_idx * 8u));
  if (PartIdDegenerate(id)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_part_ids_mutex);
  const uint32_t gen = g_perf_level_generation.load(std::memory_order_relaxed);
  if (gen != g_part_ids_generation) {
    g_culled_part_ids.clear();
    g_part_ids_generation = gen;
  }
  if (g_culled_part_ids.count(id) == 0) {
    return false;
  }
  return true;
}

}  // namespace

// Material-registration capture: the "stdx r8,r29,r9" table store in
// sub_82A30600's per-material loop (base 0x82A307FC / TU1 0x82A3F3CC).
// r8 = the 64-bit ID pair being stored, r30 = the 28-byte name record
// (already advanced: current record = r30 - 0x1C; SSO string: length @+0x14,
// inline text if < 16 else heap pointer @+0). Names matching
// dcc2_performance_cull_parts put their ID in the cull set consumed by the
// registration gates below.
void DCC2_PartMaterialCapture(PPCRegister& r8, PPCRegister& r30) {
  if (!REXCVAR_GET(dcc2_minimal_render) || PartsCullList().empty() ||
      r30.u32 < 0x1Cu + 0x10000u) {
    return;
  }
  auto* mem = REX_KERNEL_MEMORY();
  const uint32_t rec = r30.u32 - 0x1Cu;
  const uint32_t len =
      __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(rec + 0x14u));
  uint32_t name_ga = rec;
  if (len >= 0x10u) {
    name_ga = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(rec));
    if (name_ga < 0x10000u || name_ga >= 0xC0000000u) {
      return;
    }
  }
  const char* src = mem->TranslateVirtual<const char*>(name_ga);
  char name[160];
  size_t i = 0;
  for (; i < sizeof(name) - 1 && src[i] && static_cast<unsigned char>(src[i]) >= 0x20u &&
         static_cast<unsigned char>(src[i]) <= 0x7Eu;
       ++i) {
    name[i] = src[i];
  }
  name[i] = '\0';
  if (i == 0) {
    return;
  }
  if (!PartIdDegenerate(r8.u64) && PartNameMatches(name)) {
    std::lock_guard<std::mutex> lock(g_part_ids_mutex);
    const uint32_t gen = g_perf_level_generation.load(std::memory_order_relaxed);
    if (gen != g_part_ids_generation) {
      g_culled_part_ids.clear();
      g_part_ids_generation = gen;
    }
    g_culled_part_ids.insert(r8.u64);
  }
}

// LoadFacesData part-registration site A: the sub_829F1E48 call (base
// 0x82A32C50 / TU1 0x82A41820) that adds the part to the model's geometry
// object -- reached only when the geometry object exists. The part's face
// count is in r8; zeroing it registers a 0-face sub-mesh (part indexing and
// the aux table walk stay intact, nothing draws).
void DCC2_PartRegisterGateA(PPCRegister& r1, PPCRegister& r8, PPCRegister& r25) {
  if (PartIdCulled(r1.u32, r25.u32)) {
    r8.u64 = 0;
  }
}

// LoadFacesData part-registration site B: the face-count store ("sth r10")
// into the 16-byte {shader, from, count} record pushed onto the master's part
// vector (base 0x82A32C8C / TU1 0x82A4185C) -- always reached, including when
// site A is skipped.
void DCC2_PartRegisterGateB(PPCRegister& r1, PPCRegister& r10, PPCRegister& r25) {
  if (PartIdCulled(r1.u32, r25.u32)) {
    r10.u64 = 0;
  }
}

// WaterPass::Render entry (base 0x82B53148 / TU1 0x82B65310, vtbl slot 25 of
// the WaterPass RENDERPASS class). The in-level water surface is a fullscreen
// composite (water/FullScreen_Water_VS/PS + reflection RTT + GBuffer depth),
// not a mesh, so no part/model gate can remove it. The
// function's own not-yet-initialized path returns 0 without drawing, so an
// entry gate returning 0 is shipped behavior. Levels without water never
// exercise the pass; the End/cleanup vmethod (slot 26) is untouched.
bool DCC2_WaterPassGate(PPCRegister& r3) {
  if (!REXCVAR_GET(dcc2_minimal_render) || !REXCVAR_GET(dcc2_performance_cull_water)) {
    return false;
  }
  r3.u64 = 0;  // the pass's own "not drawn" return
  return true;
}

// Reflection scene-render skip (dcc2_water_reflection). The planar water
// reflection re-renders the whole scene mirrored into the ReflectionPass RTT
// -- one of the most expensive items in a frame. The render function
// (base sub_82BCD378 / TU1 sub_82BE2DA8) gates all of it on a scene-actor
// lookup: FindActorByName("world/defaultReflectionPlane"); a null result
// takes the shipped "no reflection plane" branch. This hook sits right after
// that lookup returns (base 0x82BCD6E0 / TU1 0x82BE3110, the "mr r31,r3")
// and zeroes r3. The other user of the lookup (sub_82B15F48, player-vs-water
// gameplay math) is untouched.
void DCC2_ReflectionPlaneGate(PPCRegister& r3) {
  if (REXCVAR_GET(dcc2_water_reflection)) {
    return;
  }
  if (!r3.u32) {
    return;  // no plane anyway
  }
  r3.u64 = 0;  // the function's own null-check takes the no-reflection path
}

// Shadow-map scene-draw skip (dcc2_performance_cull_shadows). ShadowPass worker
// sub_82BC78A0 (base; TU1 0x82BDD2D0) and CubeShadowPass worker sub_82BC75E8
// (TU1 0x82BDD018) share the block: [draw scene via sub_829EFA30 + camera
// callback] then surface end + EDRAM resolve. The jump hook skips only the draw
// block; the shadow target still clears and resolves every frame, but the
// cleared value samples as shadowed, so the scene renders dim. Fires once per
// shadow pass per frame; both passes share this gate.
bool DCC2_ShadowSceneSkip() {
  if (!REXCVAR_GET(dcc2_performance_cull_shadows)) {
    return false;
  }
  return true;  // jump past the scene draw
}

// sub_829DFBF0 is the boned-model skeleton draw. It reads *(*(model+288)+12)
// (r11 = *(r3+288); r10 = *(r11+12)) with no null check, and the sub-object at
// +288 is null until the avatar resources are built (e.g. the menu's
// player-avatar model, XAVT "gui/gamepad0", without avatar data). Skip the draw
// while it is null; the zero test is endian-independent.
// registers = ["r3"], return_on_true = true.
bool DCC2_SkipUnbuiltModel(PPCRegister& r3) {
  if (!r3.u32) {
    return false;
  }
  auto* mem = REX_KERNEL_MEMORY();
  const uint32_t sub_object = *mem->TranslateVirtual<const uint32_t*>(r3.u32 + 288);
  return sub_object == 0;  // true -> unbuilt model, skip the draw
}

// AVATARMODELMANAGER::OnPlayerAvatarChanged (sub_829DE170) loads and attaches
// the player avatar only if a1[a2+38] (the per-player avatar-data buffer) is
// non-null. It is allocated by RetrieveMetadata(a5=0), which is reached only
// online. Build the same 20-byte buffer sub_8236CCE8 makes ([0]=count=1,
// [4]=1000B metadata, [8]=28B, [12]=8B) and store it in a1[a2+38]; the game's
// RetrieveMetadata(a5=1) then runs -> XamAvatarGetManifestLocalUser (served from
// the captured manifest) -> the attach loop builds the render actor.
// registers = ["r3","r4"] (a1=manager, a2=player index).
void DCC2_InjectAvatarData(PPCRegister& r3, PPCRegister& r4) {
  // The forced avatar build runs at the menu; make sure the null page and
  // read-fault tolerance are armed so its reads of unready data return 0.
  static bool committed = false;
  if (!committed) {
    committed = true;
    DCC2_CommitGuestNullPage();
  }
  auto* mem = REX_KERNEL_MEMORY();
  const uint32_t a1 = r3.u32, a2 = r4.u32;
  if (a1 < 0x1000u || a2 >= 4u) {
    return;
  }
  auto rd = [&](uint32_t addr) -> uint32_t {
    return __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(addr));
  };
  auto wr = [&](uint32_t addr, uint32_t v) {
    *mem->TranslateVirtual<uint32_t*>(addr) = __builtin_bswap32(v);
  };
  const uint32_t field_addr = a1 + (a2 + 38u) * 4u;
  if (rd(field_addr) != 0) {
    return;  // already allocated -> the game's normal path handles it.
  }

  static uint32_t g_avatar_buf[4] = {0, 0, 0, 0};
  if (!g_avatar_buf[a2]) {
    const uint32_t v15 = mem->SystemHeapAlloc(20u, 0x10u);
    const uint32_t buf1000 = mem->SystemHeapAlloc(1000u, 0x10u);
    const uint32_t buf28 = mem->SystemHeapAlloc(28u, 0x10u);
    const uint32_t buf8 = mem->SystemHeapAlloc(8u, 0x10u);
    if (!v15 || !buf1000 || !buf28 || !buf8) {
      REXKRNL_WARN("[avatarinject] alloc failed");
      return;
    }
    std::memset(mem->TranslateVirtual<void*>(buf1000), 0, 1000);
    std::memset(mem->TranslateVirtual<void*>(buf28), 0, 28);
    std::memset(mem->TranslateVirtual<void*>(buf8), 0, 8);
    wr(v15 + 0u, 1u);        // count
    wr(v15 + 4u, buf1000);   // X_AVATAR_METADATA (1000B)
    wr(v15 + 8u, buf28);     // 28B
    wr(v15 + 12u, buf8);     // 8B
    wr(v15 + 16u, 0u);
    g_avatar_buf[a2] = v15;
  }
  wr(field_addr, g_avatar_buf[a2]);  // a1[a2+38] = buf -> condition passes, game loads+attaches
}

// AVATARMODELMASTER::LoadInBackground (sub_829E1718) skips the boned-model
// resource build (sub_82A7DC28) when its state *(master+712)==1 (the offline
// "loaded but don't build" state), leaving BONEDMODELRESOURCE +597 ("resources
// built") = 0 so the draw bails "resources not built". The hook forces the build
// by jumping the gate (bne @0x829E1788) to the build path loc_829E17C0.
// registers = ["r31"] (r31 = master = a1), jump_address = 0x829E17C0.
void DCC2_ForceAvatarBuild(PPCRegister& r31) {
  (void)r31;
}

// AVATARMODELMASTER::UpdateAvatarDataLoading (sub_829E05D0, r3 = master = result)
// logs "Avatar metadata not set" and can't complete the load while the master's
// own inline metadata container is empty: result[265]=count (master+1060),
// result[266]=metadata array base @1000B stride (master+1064), result[267]/[268]
// = the 28B/8B side arrays. This is separate from the manager buffer provided by
// DCC2_InjectAvatarData. With master+1064 null the resource build never runs:
// BONEDMODELRESOURCE::_BuildInstance bails ("resources not built"), no animation
// master registers, bone tags don't resolve, and the skeleton stays invisible.
// Fill the container from the cached default-avatar metadata so the load
// completes (master+712 -> 0) and the real build runs. registers = ["r3"].
void DCC2_SetMasterMetadata(PPCRegister& r3) {
  const uint32_t master = r3.u32;
  if (master < 0x1000u || master >= 0xC0000000u) {
    return;
  }
  auto* mem = REX_KERNEL_MEMORY();
  auto rd = [&](uint32_t addr) -> uint32_t {
    return __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(addr));
  };
  auto wr = [&](uint32_t addr, uint32_t v) {
    *mem->TranslateVirtual<uint32_t*>(addr) = __builtin_bswap32(v);
  };
  if (rd(master + 1064u) != 0u) {
    return;  // master metadata already set -> the game's natural path handles it.
  }
  // Cached default-avatar metadata (1000B X_AVATAR_METADATA), captured by
  // xam_avatar.cpp from the first successful GetAssets. Loaded once.
  static const std::vector<uint8_t> s_meta = [] {
    std::ifstream f("dcc2_avatar_meta.bin", std::ios::binary);
    if (!f) {
      return std::vector<uint8_t>{};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
  }();
  if (s_meta.size() < 1000u) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXKRNL_WARN("[avatarmaster] no cached metadata (dcc2_avatar_meta.bin) - run once online/"
                   "with the asset pack first to capture it");
    }
    return;
  }
  const uint32_t meta = mem->SystemHeapAlloc(1000u, 0x10u);
  const uint32_t ov = mem->SystemHeapAlloc(28u, 0x10u);
  const uint32_t ex = mem->SystemHeapAlloc(8u, 0x10u);
  if (!meta || !ov || !ex) {
    REXKRNL_WARN("[avatarmaster] alloc failed");
    return;
  }
  std::memcpy(mem->TranslateVirtual<void*>(meta), s_meta.data(), 1000u);
  std::memset(mem->TranslateVirtual<void*>(ov), 0, 28);
  std::memset(mem->TranslateVirtual<void*>(ex), 0, 8);
  wr(master + 1060u, 1u);    // result[265] = count
  wr(master + 1064u, meta);  // result[266] = metadata array base (1000B, filled)
  wr(master + 1068u, ov);    // result[267] = overlapped/handle array (28B)
  wr(master + 1072u, ex);    // result[268] = side array (8B)
}

// --- FMOD master-mix injection ---
// The LEAP mastering node (CSWOutput::Process 0x82679B68) copies one input slot into the
// renderer connection's driver buffer (conn+0x3C), but its slot index is desynced from the slot
// the FMOD mix lands in, so ReleaseDriverBuffer (0x8265DB30) silence-fills the buffer. Process
// cannot be hooked (any hook there stalls the pipeline). The render-callback thread alone owns
// the buffer between its frame-ready wait and the deinterleave (XAudio_SubmitFrameAndWait
// 0x8265DB90, post-wait @0x8265DC14), so the CSWOutput nodes are recorded at Initialize and, at
// that point, the loudest input slot is copied over the driver buffer. Guest-to-guest copy of the
// 0x1800-byte interleaved 6ch float frame; no counters/flags touched.
REXCVAR_DEFINE_BOOL(dcc2_fmod_master_inject, true, "DCC2/Audio",
                    "Deliver FMOD's mix by copying the loudest mastering-input slot "
                    "into the driver buffer inside the render callback.");

// Each renderer connection only receives its own mastering node's slots (CSWOutput field [32] @
// node+0x80 = the conn interface = conn+4). Searching all nodes would feed FMOD's mix into the
// intro/XMV engine's connection, which then plays a late, gappy duplicate of the music.

static uint32_t g_cswoutput_nodes[4] = {0, 0, 0, 0};

// CSWOutput::Initialize (0x82679C50) entry, r3 = node. One-shot per node at engine init.
void DCC2_RegisterOutputNode(PPCRegister& r3) {
  const uint32_t self = r3.u32;
  if (self < 0x1000u) {
    return;
  }
  for (auto& n : g_cswoutput_nodes) {
    if (n == self) {
      return;
    }
    if (n == 0u) {
      n = self;
      return;
    }
  }
}

// XAudio_SubmitFrameAndWait post-wait (0x8265DC14), r31 = conn base. The frame-ready
// event has fired (the producer's Process+ReleaseDriverBuffer ran this frame) and the
// driver buffer is about to be deinterleaved+submitted -- the one moment the callback
// thread owns its content outright.
void DCC2_InjectMasterMix(PPCRegister& r31) {
  if (!REXCVAR_GET(dcc2_fmod_master_inject)) {
    return;
  }
  auto* m = REX_KERNEL_MEMORY();
  auto rd = [&](uint32_t a) -> uint32_t {
    return a < 0x1000u ? 0u : __builtin_bswap32(*m->TranslateVirtual<const uint32_t*>(a));
  };
  const uint32_t dst_ga = rd(r31.u32 + 0x3Cu);
  if (dst_ga < 0x10000u) {
    return;
  }
  uint32_t* dst = m->TranslateVirtual<uint32_t*>(dst_ga);
  auto peak_of = [](const uint32_t* p) -> float {
    float pk = 0.0f;
    for (int i = 0; i < 1536; ++i) {
      const uint32_t b = __builtin_bswap32(p[i]);
      float f;
      std::memcpy(&f, &b, sizeof(f));
      const float a = f < 0.0f ? -f : f;
      if (f == f && a < 1.0e4f && a > pk) {
        pk = a;
      }
    }
    return pk;
  };
  const float dst_pk = peak_of(dst);
  if (dst_pk > 0.01f) {
    return;  // native frame live -- the engine delivered its own content
  }
  // Find this connection's own mastering node: CSWOutput field [32] (node+0x80)
  // holds the CAudioOutput/conn interface pointer, which is conn+4
  // (CXboxRendererConnection layout). Only that node's slots may be delivered
  // here (see g_cswoutput_nodes).
  uint32_t own_node = 0;
  for (const uint32_t node : g_cswoutput_nodes) {
    if (node == 0u) {
      break;
    }
    if (rd(node + 0x80u) == r31.u32 + 4u) {
      own_node = node;
      break;
    }
  }
  if (own_node == 0u) {
    return;
  }
  // Loudest valid slot of the conn's OWN node. Slot array chain matches
  // CSWOutput::Process: *(node+0x70) is the source CBuffer, *(CBuffer+0x38)
  // the {pBuffer,flags,cnt}[12B] slot-param array. The cnt sanity range
  // rejects uninitialized slots (e.g. cnt=546766854).
  const uint32_t* best = nullptr;
  float best_pk = 0.002f;
  const uint32_t arr = rd(rd(own_node + 0x70u) + 0x38u);
  if (arr >= 0x1000u) {
    for (int slot = 0; slot < 6; ++slot) {
      const uint32_t sp = arr + static_cast<uint32_t>(slot) * 12u;
      const uint32_t buf = rd(sp);
      const uint32_t cnt = rd(sp + 8u);
      if (buf < 0x10000u || buf >= 0x50000000u || cnt == 0u || cnt > 4096u) {
        continue;
      }
      const uint32_t* hp = m->TranslateVirtual<const uint32_t*>(buf);
      const float pk = peak_of(hp);
      if (pk > best_pk) {
        best_pk = pk;
        best = hp;
      }
    }
  }
  if (!best) {
    return;  // own node idle: leave the (silent) native frame alone
  }
  // Stale-producer guard, per connection: never repeat a bit-identical frame
  // (a stalled mixer leaving old data in the slot would otherwise loop the
  // same 5.3 ms slice as a buzz).
  uint64_t sig = 0;
  for (int i = 0; i < 64; ++i) {
    sig = sig * 1315423911ull + best[i];
  }
  static uint32_t sig_conns[4] = {0, 0, 0, 0};
  static uint64_t last_sigs[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) {
    if (sig_conns[i] == r31.u32 || sig_conns[i] == 0u) {
      sig_conns[i] = r31.u32;
      if (last_sigs[i] == sig) {
        return;
      }
      last_sigs[i] = sig;
      break;
    }
  }
  if (best != dst) {
    std::memcpy(dst, best, 1536u * 4u);
  }
}

// Single-threaded mixer merge. DCC2's LEAPCORE mixer runs single-threaded in ReXGlue
// (numProcessingThreads == 1, config mask 0x10). The mix pass sub_8265B3D0 gates the per-pass
// CBufferManager::MergeBuffers (sub_826598D0), which sums each voice/worker's output into the
// output node's slot and sets the primary slot index CSWOutput::Process reads, behind
// numThreads > 1, on a path that also does an infinite worker wait (Nt_WaitForSingleObject).
// Single-threaded the merge never runs and the output node reads a stale silent slot. Call the
// game's merge once per pass after the node executor returns (0x8265B4A4, r31 = mixer ctx); the
// native gate still skips its own merge and the wait.
REX_IMPORT(sub_826598D0, g_CBufferManager_MergeBuffers, void(uint32_t));

void DCC2_ForceMerge(PPCRegister& r31) {
  const uint32_t mixer = r31.u32;
  if (mixer < 0x1000u) {
    return;
  }
  auto* m = REX_KERNEL_MEMORY();
  const uint32_t num_threads =
      __builtin_bswap32(*m->TranslateVirtual<const uint32_t*>(mixer + 0x3Cu));
  if (num_threads > 1u) {
    return;  // multi-threaded path runs MergeBuffers itself
  }
  const uint32_t cbuf_mgr = __builtin_bswap32(*m->TranslateVirtual<const uint32_t*>(mixer + 0x94u));
  if (cbuf_mgr < 0x1000u) {
    return;
  }
  g_CBufferManager_MergeBuffers(cbuf_mgr);
}

// Suppress the "GAME_SERVICES_CONNECTION_LOST" reaction (sub_82B01F70). On a lost
// service (event a3==4) the handler shows "service is not available" and calls
// sub_82B762D0(10), which kicks the underlying screen back to the title; from the
// Settings "Save and Exit" flow this strands the Help & Options overlay. The hook
// sits at the branch body (0x82B01FC4, reached only when a3==4 && flag==1) and
// jumps to the handler's no-op exit (loc_82B0203C). The preceding dialog cleanup
// still runs.
void DCC2_SuppressServiceLost() {
}


#ifdef DCC2_TU1
REX_HOOK_RAW(sub_82BCAAE0) {
  const uint32_t reward = ctx.r3.u32;
  const uint32_t type = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + reward + 72u));
  // The shipped type-9 reward grants Jungle 1's split; its eligibility stays in native code.
  if (type == 9u && !cc2::kernel::xam::LgEconomyGrantItem(1036u)) return;
  __imp__sub_82BCAAE0(ctx, base);
}
#endif
