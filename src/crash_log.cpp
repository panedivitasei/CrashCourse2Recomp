// Last-chance crash logger.
//
// The runtime handles guest faults it understands; anything else dies with
// no record. This vectored handler runs last, names the faulting module,
// RVA and address, unwinds a short host stack, and passes the exception on.
// Resolve an RVA with: llvm-symbolizer --obj=crashcourse2.exe --relative-address.

#include <cstdint>
#include <cstdio>
#include <string>

#include <windows.h>

#include <rex/logging.h>

namespace {

std::string Describe(uint64_t addr) {
  HMODULE mod = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCSTR>(addr), &mod);
  char name[MAX_PATH] = {0};
  if (mod) {
    GetModuleFileNameA(mod, name, MAX_PATH);
  }
  const char* base_name = name;
  for (const char* p = name; *p; ++p) {
    if (*p == 92 || *p == 47) base_name = p + 1;  // backslash or slash
  }
  char out[MAX_PATH + 48];
  const uint64_t base = reinterpret_cast<uint64_t>(mod);
  std::snprintf(out, sizeof(out), "%s+0x%llx", mod ? base_name : "?",
                static_cast<unsigned long long>(mod ? addr - base : addr));
  return out;
}

LONG CALLBACK LastChance(EXCEPTION_POINTERS* info) {
  const auto* rec = info->ExceptionRecord;
  const auto code = rec->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
      code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const uint64_t rip = info->ContextRecord->Rip;
  const uint64_t fault =
      code == EXCEPTION_ACCESS_VIOLATION ? static_cast<uint64_t>(rec->ExceptionInformation[1]) : 0;
  REXLOG_ERROR("[crash] {:#x} at {} fault_addr={:#x}", static_cast<uint32_t>(code), Describe(rip),
               fault);

  // Frame-pointer-free unwind through the function tables.
  CONTEXT ctx = *info->ContextRecord;
  for (int frame = 0; frame < 24 && ctx.Rip; ++frame) {
    DWORD64 image_base = 0;
    auto* fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (!fn) {
      break;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher,
                     nullptr);
    if (!ctx.Rip) {
      break;
    }
    REXLOG_ERROR("[crash]   #{:<2} {}", frame + 1, Describe(ctx.Rip));
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// Registered last (FirstHandler = 0) so the runtime's own handlers keep
// their first look at guest faults.
struct Install {
  Install() { AddVectoredExceptionHandler(0, &LastChance); }
} g_install;

}  // namespace
