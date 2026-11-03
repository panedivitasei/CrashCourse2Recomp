// Main-menu map reload after its actors are deleted, and the scene path handed to the avatar light on SCENE::Load.

#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "cc2_init.h"
#include "cc2/avatar_light.h"

REXCVAR_DEFINE_BOOL(cc2_menu_map_reload, true, "Gameplay",
                    "Reload the main-menu map after its actors are deleted, instead of showing an empty menu");

#ifdef DCC2_TU1
namespace {
uint32_t ReadSceneWord(uint8_t* base, uint32_t address) {
  return *reinterpret_cast<const rex::be<uint32_t>*>(base + address);
}
std::string ScenePath(uint8_t* base, uint32_t address) {
  if (address < 0x10000 || address >= 0xFFFFFE00) return "<null>";
  std::string result;
  for (unsigned i = 0; i < 256 && base[address + i]; ++i) {
    const auto ch = base[address + i];
    if (ch < 32 || ch > 126) return "<invalid>";
    result += static_cast<char>(ch);
  }
  return result;
}
// GUIXA::LoadMap skips loading while GUIXA+32884 is set, and deleting the menu actors (DeleteMenuMapActors, or a
// course unload's SCENE::DeleteAllActors) leaves it set; clearing it lets the map load again instead of leaving the
// menu UI over an empty scene.
void ForgetMenuMap(uint8_t* base) {
  if (!REXCVAR_GET(cc2_menu_map_reload)) return;
  const auto gui = ReadSceneWord(base, 0x82E993F4);
  if (gui) base[gui + 32884] = 0;
}
}  // namespace

// SCENE::Load
REX_HOOK_RAW(sub_82AC8AD0) {
  cc2::avatar_light::SceneLoaded(ScenePath(base, ctx.r4.u32));
  __imp__sub_82AC8AD0(ctx, base);
}

// GUIXA::DeleteMenuMapActors
REX_HOOK_RAW(sub_82B83900) {
  __imp__sub_82B83900(ctx, base);
  ForgetMenuMap(base);
}

// SCENE::DeleteAllActors
REX_HOOK_RAW(sub_82AC7E80) {
  const bool main_scene = ctx.r3.u32 == ReadSceneWord(base, 0x82EDBB1C);
  __imp__sub_82AC7E80(ctx, base);
  if (main_scene) ForgetMenuMap(base);
}
#endif
