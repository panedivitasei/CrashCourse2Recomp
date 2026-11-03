// Content packages, title-side.
//
// Installed packages (patches, the Hong Kong DLC) live under the common
// content root (xuid 0). The enumerator lists them, but the runtime re-opens
// content strictly under the signed-in profile's xuid, so the re-open fails,
// the additional-content record ends badly and the game tries to download a
// pak nothing serves. Re-resolve to the common root when the package is not
// under the profile.

#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/hook.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace rex::kernel::xam {

using namespace rex;
using namespace rex::system;
using namespace rex::system::xam;

namespace {

enum class kDispositionState : uint32_t { Unknown = 0, Create = 1, Open = 2 };

// The profile's xuid, unless the package only exists as common content.
uint64_t ResolveXuid(const XCONTENT_AGGREGATE_DATA& content_data) {
  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  if (content_data.content_type == XContentType::kMarketplaceContent) {
    return 0;
  }
  auto* content_manager = REX_KERNEL_STATE()->content_manager();
  if (xuid != 0 && !content_manager->ContentExists(xuid, content_data) &&
      content_manager->ContentExists(0, content_data)) {
    return 0;
  }
  return xuid;
}

u32 ContentCreate(u32 user_index, mapped_string root_name, mapped_void content_data_ptr,
                  u32 content_data_size, u32 flags, mapped_u32 disposition_ptr,
                  mapped_u32 license_mask_ptr, mapped_void overlapped_ptr) {
  XCONTENT_AGGREGATE_DATA content_data;
  if (content_data_size == sizeof(XCONTENT_DATA)) {
    content_data = *content_data_ptr.as<XCONTENT_DATA*>();
  } else if (content_data_size == sizeof(XCONTENT_AGGREGATE_DATA)) {
    content_data = *content_data_ptr.as<XCONTENT_AGGREGATE_DATA*>();
  } else {
    return X_ERROR_INVALID_PARAMETER;
  }
  const uint64_t xuid = ResolveXuid(content_data);
  auto* content_manager = REX_KERNEL_STATE()->content_manager();

  if (overlapped_ptr && disposition_ptr) {
    *disposition_ptr = 0;
  }

  auto run = [content_manager, xuid, root_name = std::string(root_name.value()), flags,
              content_data, disposition_ptr,
              license_mask_ptr](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result = X_ERROR_INVALID_PARAMETER;
    kDispositionState disposition = kDispositionState::Unknown;
    switch (flags & 0xF) {
      case 1:  // CREATE_NEW
        if (content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_ALREADY_EXISTS;
        } else {
          disposition = kDispositionState::Create;
        }
        break;
      case 2:  // CREATE_ALWAYS
        content_manager->CloseContent(root_name);
        if (content_manager->ContentExists(xuid, content_data)) {
          content_manager->DeleteContent(xuid, content_data);
        }
        disposition = content_manager->ContentExists(xuid, content_data)
                          ? kDispositionState::Open
                          : kDispositionState::Create;
        break;
      case 3:  // OPEN_EXISTING
        if (!content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_PATH_NOT_FOUND;
        } else {
          disposition = kDispositionState::Open;
        }
        break;
      case 4:  // OPEN_ALWAYS
        disposition = content_manager->ContentExists(xuid, content_data)
                          ? kDispositionState::Open
                          : kDispositionState::Create;
        break;
      case 5:  // TRUNCATE_EXISTING
        if (!content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_PATH_NOT_FOUND;
        } else {
          content_manager->CloseContent(root_name);
          content_manager->DeleteContent(xuid, content_data);
          disposition = content_manager->ContentExists(xuid, content_data)
                            ? kDispositionState::Open
                            : kDispositionState::Create;
        }
        break;
      default:
        break;
    }

    uint32_t content_license = 0;
    if (disposition == kDispositionState::Create) {
      result = content_manager->CreateContent(root_name, xuid, content_data);
      if (XSUCCEEDED(result)) {
        content_manager->WriteContentHeaderFile(xuid, content_data);
      }
    } else if (disposition == kDispositionState::Open) {
      result = content_manager->OpenContent(root_name, xuid, content_data, content_license);
    }
    if (license_mask_ptr && XSUCCEEDED(result)) {
      *license_mask_ptr = content_license;
    }
    if (disposition_ptr) {
      *disposition_ptr = static_cast<uint32_t>(disposition);
    }
    REXKRNL_DEBUG("[content] create '{}' file='{}' type={:#x} flags={:#x} xuid={:016X} -> {:#x} disp={}",
                  root_name, content_data.file_name(),
                  static_cast<uint32_t>(static_cast<XContentType>(content_data.content_type)), flags, xuid,
                  static_cast<uint32_t>(result), static_cast<uint32_t>(disposition));
    extended_error = X_HRESULT_FROM_WIN32(result);
    length = static_cast<uint32_t>(disposition);
    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error = 0;
    uint32_t length = 0;
    return run(extended_error, length);
  }
  REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped_ptr.guest_address());
  return X_ERROR_IO_PENDING;
}

}  // namespace

u32 XamContentCreateEx_entry(u32 user_index, mapped_string root_name, mapped_void content_data_ptr,
                             u32 flags, mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                             u32 cache_size, u64 content_size, mapped_void overlapped_ptr) {
  return ContentCreate(user_index, root_name, content_data_ptr, sizeof(XCONTENT_DATA), flags,
                       disposition_ptr, license_mask_ptr, overlapped_ptr);
}

u32 XamContentCreate_entry(u32 user_index, mapped_string root_name, mapped_void content_data_ptr,
                           u32 flags, mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                           mapped_void overlapped_ptr) {
  return ContentCreate(user_index, root_name, content_data_ptr, sizeof(XCONTENT_DATA), flags,
                       disposition_ptr, license_mask_ptr, overlapped_ptr);
}

u32 XamContentCreateInternal_entry(mapped_string root_name, mapped_void content_data_ptr, u32 flags,
                                   mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                                   u32 cache_size, u64 content_size, mapped_void overlapped_ptr) {
  return ContentCreate(0xFE, root_name, content_data_ptr, sizeof(XCONTENT_AGGREGATE_DATA), flags,
                       disposition_ptr, license_mask_ptr, overlapped_ptr);
}

u32 XamContentGetCreator_entry(u32 user_index, mapped_void content_data_ptr,
                               mapped_u32 is_creator_ptr, mapped_u64 creator_xuid_ptr,
                               mapped_void overlapped_ptr) {
  auto result = X_ERROR_SUCCESS;
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();
  const uint64_t xuid = ResolveXuid(content_data);
  const bool content_exists =
      REX_KERNEL_STATE()->content_manager()->ContentExists(xuid, content_data);
  if (content_exists) {
    const bool own_save = content_data.content_type == XContentType::kSavedGame && xuid != 0;
    *is_creator_ptr = own_save ? 1 : 0;
    if (creator_xuid_ptr) {
      *creator_xuid_ptr = own_save ? xuid : 0;
    }
  } else {
    result = X_ERROR_PATH_NOT_FOUND;
  }
  REXKRNL_DEBUG("[content] creator file='{}' type={:#x} xuid={:016X} exists={}",
                content_data.file_name(), static_cast<uint32_t>(static_cast<XContentType>(content_data.content_type)), xuid,
                content_exists);
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  }
  return result;
}

}  // namespace rex::kernel::xam

REX_EXPORT(__imp__XamContentCreateEx, rex::kernel::xam::XamContentCreateEx_entry)
REX_EXPORT(__imp__XamContentCreate, rex::kernel::xam::XamContentCreate_entry)
REX_EXPORT(__imp__XamContentCreateInternal, rex::kernel::xam::XamContentCreateInternal_entry)
REX_EXPORT(__imp__XamContentGetCreator, rex::kernel::xam::XamContentGetCreator_entry)
