// NtOpenFile, title-side.
//
// During boot the game polls game:\data\templates\dlc_hongkong\ a few hundred times; Hong Kong
// ships inside its DLC package, so the folder never exists there and the runtime logs every miss.
// That one path gets the runtime's own not-found answer here; everything else goes to the runtime.

#include <cstdint>
#include <cstring>
#include <string_view>

#include <windows.h>

#include <rex/filesystem/entry.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xio.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

using rex::X_HANDLE;
using rex::X_STATUS;

namespace {

PPCFunc* OriginalExport(const char* name) {
  HMODULE runtime = GetModuleHandleW(L"rexruntimerd.dll");
  if (!runtime) runtime = GetModuleHandleW(L"rexruntime.dll");
  if (!runtime) return nullptr;
  return reinterpret_cast<PPCFunc*>(GetProcAddress(runtime, name));
}

constexpr std::string_view kHongKongTemplates = "game:\\data\\templates\\dlc_hongkong\\";

template <typename T>
T* Guest(uint32_t address) {
  return address ? REX_KERNEL_MEMORY()->TranslateVirtual<T*>(address) : nullptr;
}

bool IsHongKongTemplates(uint32_t object_attributes) {
  const auto* attrs = Guest<const rex::system::X_OBJECT_ATTRIBUTES>(object_attributes);
  if (!attrs) return false;
  const auto* name = Guest<const rex::system::X_ANSI_STRING>(attrs->name_ptr);
  const char* chars = name ? Guest<const char>(name->pointer) : nullptr;
  if (!chars) return false;
  // The runtime trims the name before resolving it.
  std::string_view path(chars, name->length);
  while (!path.empty() && (path.back() == '\0' || path.back() == ' ')) path.remove_suffix(1);
  while (!path.empty() && path.front() == ' ') path.remove_prefix(1);
  return path.size() == kHongKongTemplates.size() &&
         _strnicmp(path.data(), kHongKongTemplates.data(), path.size()) == 0;
}

}  // namespace

// NtOpenFile(handle_out, desired_access, object_attributes, io_status_block, open_options).
REX_HOOK_RAW(__imp__NtOpenFile) {
  if (IsHongKongTemplates(ctx.r5.u32)) {
    if (auto* io = Guest<rex::system::X_IO_STATUS_BLOCK>(ctx.r6.u32)) {
      io->status = X_STATUS_NO_SUCH_FILE;
      io->information = static_cast<uint32_t>(rex::filesystem::FileAction::kDoesNotExist);
    }
    if (auto* handle = Guest<rex::be<uint32_t>>(ctx.r3.u32)) *handle = X_INVALID_HANDLE_VALUE;
    ctx.r3.u64 = X_STATUS_NO_SUCH_FILE;
    return;
  }
  static PPCFunc* original = OriginalExport("__imp__NtOpenFile");
  if (!original) {
    ctx.r3.u64 = X_STATUS_UNSUCCESSFUL;
    return;
  }
  original(ctx, base);
}
