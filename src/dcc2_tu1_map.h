// dcc2_tu1_map.h - base-image vs TU1 guest address selection.
//
// TU1 is a full relink: functions shift variably and .data/bss moved
// non-uniformly, so every guest address the C++ side touches is translated
// here. The repo builds the TU1 image, so CMakeLists.txt defines DCC2_TU1
// unconditionally; the base half is kept for reference.
//
// kGA_<basehex> constants: guest addresses (data globals / mid-function code
// addresses). sub_ #defines: guest function aliases so DECLARE_REX_FUNC and
// call sites resolve to the TU tree's symbols.
#pragma once
#include <cstdint>

#ifdef DCC2_TU1

inline constexpr uint32_t kGA_82175F14 = 0x82176DF0u;  // .rdata theme name "jungle"
inline constexpr uint32_t kGA_82175F1C = 0x82176DF8u;  // .rdata theme name "arctic"
inline constexpr uint32_t kGA_82175F24 = 0x82176E00u;  // .rdata theme name "desert"
inline constexpr uint32_t kGA_82175F2C = 0x82176E08u;  // .rdata theme name "island"
inline constexpr uint32_t kGA_822F2A28 = 0x823EE7E0u;  // benign "li r3,2; blr" fn (TU: equivalent instance; original stripped)
inline constexpr uint32_t kGA_82AB81A8 = 0x82AC7528u;  // text fn used as pointer
inline constexpr uint32_t kGA_82AF6060 = 0x82B044B8u;  // li r4,0 site in WT level loader (= DCC2_LoaderLevelOverride hook)
inline constexpr uint32_t kGA_82B633A4 = 0x82B75928u;  // ForceLoaderExit hook site in GAMEXA::UpdateLoading
inline constexpr uint32_t kGA_82B633D8 = 0x82B7595Cu;  // ForceLoaderExit jump target
inline constexpr uint32_t kGA_82B63D40 = 0x82B762CCu;  // SkipLoadReshow hook site
inline constexpr uint32_t kGA_82CDD740 = 0x82CF3740u;  // code-region upper bound heuristic (embsec+0x4B40)
inline constexpr uint32_t kGA_82D7C13E = 0x82D9CA0Eu;
inline constexpr uint32_t kGA_82D7FF30 = 0x82DA0944u;
inline constexpr uint32_t kGA_82D85688 = 0x82D85688u;  // diag-only navbind record (SetLvlConfAll); base address, not translated
inline constexpr uint32_t kGA_82D85828 = 0x82D85828u;  // diag-only navbind record (NavLvlConfReset); base address, not translated
inline constexpr uint32_t kGA_82D858A8 = 0x82D858A8u;  // diag-only navbind record (NavLvlConfThemeTotal); base address, not translated
inline constexpr uint32_t kGA_82D88118 = 0x82D88118u;  // diag-only navbind record (ProfileGetIsUnlocked); base address, not translated
inline constexpr uint32_t kGA_82E7338C = 0x82E98B1Cu;
inline constexpr uint32_t kGA_82E73390 = 0x82E98B20u;
inline constexpr uint32_t kGA_82E73398 = 0x82E98B28u;  // level mgr ptr
inline constexpr uint32_t kGA_82E7339C = 0x82E98B2Cu;
inline constexpr uint32_t kGA_82E733A0 = 0x82E98B30u;  // theme mgr ptr
inline constexpr uint32_t kGA_82E73C5C = 0x82E993ECu;  // deferred light-mgr ptr (light list at +300)
inline constexpr uint32_t kGA_82E73C64 = 0x82E993F4u;
inline constexpr uint32_t kGA_82E84B90 = 0x82EAA320u;
inline constexpr uint32_t kGA_82E84B98 = 0x82EAA328u;  // active scene dword
inline constexpr uint32_t kGA_82EB5288 = 0x82EDAA18u;  // AVATARMODELMANAGER singleton (tu_data 6/6, +0x25790 like neighbors)
inline constexpr uint32_t kGA_82EB52B4 = 0x82EDAA44u;  // AdditionalContentXA singleton ptr (bss; neighbors 52B8/52DC both +0x25790)
inline constexpr uint32_t kGA_82EB52B8 = 0x82EDAA48u;
inline constexpr uint32_t kGA_82EB52DC = 0x82EDAA6Cu;
inline constexpr uint32_t kGA_82EB52F8 = 0x82EDAA88u;  // ENVIROMENT/scene-settings ptr (postfx flags @+1876; +0x25790 like neighbors)
inline constexpr uint32_t kGA_82EB6328 = 0x82EDBAF0u;
inline constexpr uint32_t kGA_82EB6B58 = 0x82EDAAE4u;  // bss reordered in TU (not +uniform)
inline constexpr uint32_t kGA_82EB6B5C = 0x82EDBB1Cu;
inline constexpr uint32_t kGA_82EB6B80 = 0x82EDC340u;
// Deferred pass-bracket bytes (avatar technique selectors in
// DEFERREDAVATARMODELRESOURCE::LoadAndSetAvatarShader 0x82BC6660). The TU
// relink scattered them rather than shifting them by +0x257C0; addresses are
// the lis -32018 / lbz pairs in the TU pass wrappers and dispatch sub_829EA338.
inline constexpr uint32_t kGA_82EB7CA8 = 0x82EDC36Au;  // GBuffer bracket byte
inline constexpr uint32_t kGA_82EB7CAA = 0x82EDD46Cu;  // light-prepass main bracket byte (sub_829DBEA0)
inline constexpr uint32_t kGA_82EB7CAB = 0x82EDD46Du;  // ZPrePass bracket byte (sub_82BCCC20)
inline constexpr uint32_t kGA_82EB7CB8 = 0x82EDD478u;
inline constexpr uint32_t kGA_82EB7CD0 = 0x82EDD490u;
inline constexpr uint32_t kGA_82EBB258 = 0x82EE0A18u;  // racer obj array
inline constexpr uint32_t kGA_82EBB270 = 0x82EE0A30u;  // racer count (= array+0x18, layout-relative)
inline constexpr uint32_t kGA_82EBD2A4 = 0x82EE2A64u;
inline constexpr uint32_t kGA_82EBD2A8 = 0x82EE2A68u;
inline constexpr uint32_t kGA_82EBD2AC = 0x82EE2A6Cu;
inline constexpr uint32_t kGA_82EBD2B0 = 0x82EE2A70u;
inline constexpr uint32_t kGA_82EBD2B4 = 0x82EE2A74u;

#define sub_822409E8 sub_82243100  // exact
#define sub_8228D600 sub_822930A8  // graph-weak
#define sub_82317BE8 sub_82326638  // exact
#define sub_82317C58 sub_823266A8  // graph-weak
#define sub_824C8BF8 sub_824D8318  // exact
#define sub_824C9300 sub_824D8A20  // graph-weak
#define sub_824CA4C0 sub_824D9D38  // graph-weak
#define sub_8264B390 sub_8265A660  // order
#define sub_826598D0 sub_82667CF8  // exact
#define sub_8265DFD8 sub_8266C120  // order
#define sub_829DD768 sub_829EC9B0  // graph-weak (64 insns both sides, same +1060 metadata field)
#define sub_829DFBF0 sub_829EEE38  // order
#define sub_82A8E8D0 sub_82A9BD88  // order
#define sub_82AB8830 sub_82AC7BB0  // order
#define sub_82AE09D8 sub_82AEEB38  // order-weak
#define sub_82AE16D8 sub_82AEF838  // graph-weak
#define sub_82AE5F80 sub_82AF4030  // order
#define sub_82AE6BC8 sub_82AF4C20  // order
#define sub_82AE7210 sub_82AF5268  // order
#define sub_82AECD58 sub_82AFB0B0  // order
#define sub_82AECF20 sub_82AFB278  // order
#define sub_82AED0A0 sub_82AFB3F8  // order
#define sub_82AED2D8 sub_82AFB630  // order
#define sub_82AF4070 sub_82B00BC8  // graph-weak
#define sub_82AF4460 sub_82B02878  // order-weak
#define sub_82AF5FC0 sub_82B043F0  // resolve-window
#define sub_82B16168 sub_82B27450  // graph-weak
#define sub_82B2AA88 sub_82B3C3C0  // graph-weak
#define sub_82B327E0 sub_82B44368  // order
#define sub_82B341E0 sub_82B45E58  // resolve-caller
#define sub_82B34A58 sub_82B46640  // resolve-caller
#define sub_82B34FC0 sub_82B465B0  // resolve-window
#define sub_82B54CD0 sub_82B66EA8  // resolve-window-low
#define sub_82B557C8 sub_82B67980  // exact
#define sub_82B59E78 sub_82B6BDE0  // order-weak
#define sub_82B5B3B0 sub_82B6D4C0  // order-weak
#define sub_82B5D470 sub_82B6F200  // graph-weak
#define sub_82B60008 sub_82B72138  // order-weak
#define sub_82B718E0 sub_82B851C0  // exact
#define sub_82B9D298 sub_82BB1050  // graph-changed
#define sub_82BBA458 sub_82BCFCE0  // order-weak

#else  // base image v0.0.0.5

inline constexpr uint32_t kGA_82175F14 = 0x82175F14u;
inline constexpr uint32_t kGA_82175F1C = 0x82175F1Cu;
inline constexpr uint32_t kGA_82175F24 = 0x82175F24u;
inline constexpr uint32_t kGA_82175F2C = 0x82175F2Cu;
inline constexpr uint32_t kGA_822F2A28 = 0x822F2A28u;
inline constexpr uint32_t kGA_82AB81A8 = 0x82AB81A8u;
inline constexpr uint32_t kGA_82AF6060 = 0x82AF6060u;
inline constexpr uint32_t kGA_82B633A4 = 0x82B633A4u;
inline constexpr uint32_t kGA_82B633D8 = 0x82B633D8u;
inline constexpr uint32_t kGA_82B63D40 = 0x82B63D40u;
inline constexpr uint32_t kGA_82CDD740 = 0x82CDD740u;
inline constexpr uint32_t kGA_82D7C13E = 0x82D7C13Eu;
inline constexpr uint32_t kGA_82D7FF30 = 0x82D7FF30u;
inline constexpr uint32_t kGA_82D85688 = 0x82D85688u;
inline constexpr uint32_t kGA_82D85828 = 0x82D85828u;
inline constexpr uint32_t kGA_82D858A8 = 0x82D858A8u;
inline constexpr uint32_t kGA_82D88118 = 0x82D88118u;
inline constexpr uint32_t kGA_82E7338C = 0x82E7338Cu;
inline constexpr uint32_t kGA_82E73390 = 0x82E73390u;
inline constexpr uint32_t kGA_82E73398 = 0x82E73398u;
inline constexpr uint32_t kGA_82E7339C = 0x82E7339Cu;
inline constexpr uint32_t kGA_82E733A0 = 0x82E733A0u;
inline constexpr uint32_t kGA_82E73C5C = 0x82E73C5Cu;  // deferred light-mgr ptr (light list at +300)
inline constexpr uint32_t kGA_82E73C64 = 0x82E73C64u;
inline constexpr uint32_t kGA_82E84B90 = 0x82E84B90u;
inline constexpr uint32_t kGA_82E84B98 = 0x82E84B98u;
inline constexpr uint32_t kGA_82EB5288 = 0x82EB5288u;  // AVATARMODELMANAGER singleton
inline constexpr uint32_t kGA_82EB52B4 = 0x82EB52B4u;
inline constexpr uint32_t kGA_82EB52B8 = 0x82EB52B8u;
inline constexpr uint32_t kGA_82EB52DC = 0x82EB52DCu;
inline constexpr uint32_t kGA_82EB52F8 = 0x82EB52F8u;
inline constexpr uint32_t kGA_82EB6328 = 0x82EB6328u;
inline constexpr uint32_t kGA_82EB6B58 = 0x82EB6B58u;
inline constexpr uint32_t kGA_82EB6B5C = 0x82EB6B5Cu;
inline constexpr uint32_t kGA_82EB6B80 = 0x82EB6B80u;
inline constexpr uint32_t kGA_82EB7CA8 = 0x82EB7CA8u;  // GBuffer bracket byte
inline constexpr uint32_t kGA_82EB7CAA = 0x82EB7CAAu;  // light-prepass main bracket byte
inline constexpr uint32_t kGA_82EB7CAB = 0x82EB7CABu;  // ZPrePass bracket byte
inline constexpr uint32_t kGA_82EB7CB8 = 0x82EB7CB8u;
inline constexpr uint32_t kGA_82EB7CD0 = 0x82EB7CD0u;
inline constexpr uint32_t kGA_82EBB258 = 0x82EBB258u;
inline constexpr uint32_t kGA_82EBB270 = 0x82EBB270u;
inline constexpr uint32_t kGA_82EBD2A4 = 0x82EBD2A4u;
inline constexpr uint32_t kGA_82EBD2A8 = 0x82EBD2A8u;
inline constexpr uint32_t kGA_82EBD2AC = 0x82EBD2ACu;
inline constexpr uint32_t kGA_82EBD2B0 = 0x82EBD2B0u;
inline constexpr uint32_t kGA_82EBD2B4 = 0x82EBD2B4u;

#endif  // DCC2_TU1
