// Ordinal lookups (GetProcAddressByOrdinal) search the runtime DLL's own copy of the PPC function
// registry, which never sees the exe's REX_EXPORTs, so its entries get re-pointed at them.

#include "kernel/export_override.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <windows.h>

#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

// Import overrides bound by REX_HOOK_RAW (KeInsertQueueDpc lives in src/gpu/edge.cpp, NtOpenFile in src/kernel/xboxkrnl/nt_open_file.cpp); they reach static imports only.
extern "C" {
REX_FUNC(__imp__ExCreateThread);
REX_FUNC(__imp__XamLoaderTerminateTitle);
REX_FUNC(__imp__XamLoaderLaunchTitle);
REX_FUNC(__imp__XamLoaderLaunchTitleEx);
REX_FUNC(__imp__XamNotifyCreateListener);
REX_FUNC(__imp__XamNotifyCreateListenerInternal);
REX_FUNC(__imp__KeInsertQueueDpc);
REX_FUNC(__imp__NtOpenFile);
}

namespace cc2 {

namespace {

using Registry = std::vector<std::pair<const char*, PPCFunc*>>;

// The function-local static inside rex::ppc::GetPPCFuncRegistry(), as the
// runtime DLL exports it. Same compiler and CRT on both sides, so the
// vector layout matches.
constexpr const char* kRuntimeRegistry =
    "?registry@?1??GetPPCFuncRegistry@ppc@rex@@YAAEAV?$vector@U?$pair@PEBDP6AXAEAUPPCContext@@PEAE@Z"
    "@std@@V?$allocator@U?$pair@PEBDP6AXAEAUPPCContext@@PEAE@Z@std@@@2@@std@@XZ@4V45@A";

Registry* RuntimeRegistry() {
  HMODULE runtime = GetModuleHandleW(L"rexruntimerd.dll");
  if (!runtime) runtime = GetModuleHandleW(L"rexruntime.dll");
  if (!runtime) return nullptr;
  return reinterpret_cast<Registry*>(GetProcAddress(runtime, kRuntimeRegistry));
}

// The runtime's own implementation of each promoted name, kept for the self-check.
std::unordered_map<std::string, PPCFunc*> g_runtime_originals;

}  // namespace

void PromoteTitleExports() {
  Registry* runtime = RuntimeRegistry();
  if (!runtime) {
    REXLOG_WARN("[exports] runtime registry not found; ordinal lookups keep the runtime's exports");
    return;
  }
  int replaced = 0;
  int added = 0;
  for (const auto& [name, func] : rex::ppc::GetPPCFuncRegistry()) {
    bool hit = false;
    for (auto& entry : *runtime) {
      if (std::strcmp(entry.first, name) == 0) {
        g_runtime_originals.emplace(name, entry.second);
        entry.second = func;
        hit = true;
        ++replaced;
      }
    }
    if (!hit) {
      runtime->emplace_back(name, func);
      ++added;
    }
  }
  REXLOG_DEBUG("[exports] {} runtime entries re-pointed at the title, {} added", replaced, added);
}

namespace {

bool InExe(const void* p) {
  auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  auto* q = reinterpret_cast<const uint8_t*>(p);
  return q >= base && q < base + nt->OptionalHeader.SizeOfImage;
}

// An x64 import stub is `jmp [rip+disp32]`; returns the DLL function it jumps to.
const void* ImportStubTarget(const void* fn) {
  auto* p = reinterpret_cast<const uint8_t*>(fn);
  if (p[0] != 0xFF || p[1] != 0x25) return nullptr;
  int32_t disp;
  std::memcpy(&disp, p + 2, sizeof(disp));
  return *reinterpret_cast<const void* const*>(p + 6 + disp);
}

}  // namespace

void LogOverrideCheck(rex::Runtime* runtime) {
  // Guest thunk per host function, plus the DLL targets of any import stubs the linker used instead.
  std::unordered_map<PPCFunc*, uint32_t> thunk;
  std::unordered_set<const void*> stub_targets;
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
    thunk.emplace(m->host, static_cast<uint32_t>(m->guest));
    if (const void* target = ImportStubTarget(reinterpret_cast<const void*>(m->host))) {
      stub_targets.insert(target);
    }
  }
  auto* dispatcher = runtime->function_dispatcher();
  Registry* runtime_registry = RuntimeRegistry();
  auto runtime_entry = [&](const char* name) -> PPCFunc* {
    if (runtime_registry) {
      for (auto& [n, f] : *runtime_registry) {
        if (std::strcmp(n, name) == 0) return f;
      }
    }
    return nullptr;
  };
  enum class Bind { kTitle, kRuntime, kNotImported };
  auto bind = [&](PPCFunc* func, PPCFunc* original) {
    auto it = thunk.find(func);
    if (it != thunk.end() && dispatcher->GetFunction(it->second) == func) return Bind::kTitle;
    if (original && (stub_targets.count(reinterpret_cast<const void*>(original)) ||
                     thunk.count(original))) {
      return Bind::kRuntime;
    }
    return Bind::kNotImported;
  };

  int bound = 0, ordinal_only = 0, failed = 0;
  auto check = [&](const char* kind, const char* name, PPCFunc* func, PPCFunc* original) {
    switch (bind(func, original)) {
      case Bind::kTitle: ++bound; break;
      case Bind::kNotImported: ++ordinal_only; break;
      case Bind::kRuntime:
        ++failed;
        REXLOG_ERROR("[overrides] {} {} is statically bound to the runtime, not the title", kind, name);
        break;
    }
  };
  for (const auto& [name, func] : rex::ppc::GetPPCFuncRegistry()) {
    auto orig = g_runtime_originals.find(name);
    check("export", name, func, orig == g_runtime_originals.end() ? nullptr : orig->second);
    if (runtime_entry(name) != func) {
      ++failed;
      REXLOG_ERROR("[overrides] export {} is not promoted for ordinal lookups", name);
    }
  }
  const std::pair<const char*, PPCFunc*> raw_hooks[] = {
      {"__imp__ExCreateThread", __imp__ExCreateThread},
      {"__imp__XamLoaderTerminateTitle", __imp__XamLoaderTerminateTitle},
      {"__imp__XamLoaderLaunchTitle", __imp__XamLoaderLaunchTitle},
      {"__imp__XamLoaderLaunchTitleEx", __imp__XamLoaderLaunchTitleEx},
      {"__imp__XamNotifyCreateListener", __imp__XamNotifyCreateListener},
      {"__imp__XamNotifyCreateListenerInternal", __imp__XamNotifyCreateListenerInternal},
      {"__imp__KeInsertQueueDpc", __imp__KeInsertQueueDpc},
      {"__imp__NtOpenFile", __imp__NtOpenFile},
  };
  for (const auto& [name, func] : raw_hooks) {
    check("import hook", name, func, runtime_entry(name));
  }

  const bool resolver = InExe(reinterpret_cast<void*>(&rex::runtime::ResolveIndirectFunction));
  if (!resolver) {
    ++failed;
    REXLOG_ERROR("[overrides] ResolveIndirectFunction resolves to the runtime, invalid calls will trap");
  }
  REXLOG_DEBUG("[overrides] {} bound to the title, {} not imported by the game (ordinal only), {} failed",
              bound, ordinal_only, failed);
}

}  // namespace cc2
