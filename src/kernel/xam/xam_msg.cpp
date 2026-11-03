// Overlapped message requests, title-side.
//
// TU1 cancels a never-started async op during boot content enumeration and
// passes a null overlapped; real XAM tolerates that where the runtime faults.
// Same bodies as the runtime with the null checks the console applies.

#include <rex/hook.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xio.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace rex::kernel::xam {

using namespace rex::system;

u32 XMsgCancelIORequest_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, u32 wait) {
  if (!overlapped_ptr) {
    return 0;
  }
  X_HANDLE event_handle = XOverlappedGetEvent(overlapped_ptr);
  if (event_handle && wait) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Wait(0, 0, true, nullptr);
    }
  }
  return 0;
}

u32 XMsgCompleteIORequest_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, u32 result,
                                u32 extended_error, u32 length) {
  if (!overlapped_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(overlapped_ptr.guest_address(), result,
                                                    extended_error, length);
  return X_ERROR_SUCCESS;
}

u32 XamGetOverlappedResult_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, mapped_u32 length_ptr,
                                 u32 unknown) {
  if (!overlapped_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  uint32_t result;
  if (overlapped_ptr->result != X_ERROR_IO_PENDING) {
    result = overlapped_ptr->result;
  } else if (!overlapped_ptr->event) {
    result = X_ERROR_IO_INCOMPLETE;
  } else {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(overlapped_ptr->event);
    if (!ev) {
      return X_ERROR_IO_INCOMPLETE;
    }
    result = ev->Wait(3, 1, 0, nullptr);
    if (XSUCCEEDED(result)) {
      result = overlapped_ptr->result;
    } else {
      result = xboxkrnl::xeRtlNtStatusToDosError(result);
    }
  }
  if (XSUCCEEDED(result) && length_ptr) {
    *length_ptr = overlapped_ptr->length;
  }
  return result;
}

}  // namespace rex::kernel::xam

REX_EXPORT(__imp__XMsgCancelIORequest, rex::kernel::xam::XMsgCancelIORequest_entry)
REX_EXPORT(__imp__XMsgCompleteIORequest, rex::kernel::xam::XMsgCompleteIORequest_entry)
REX_EXPORT(__imp__XamGetOverlappedResult, rex::kernel::xam::XamGetOverlappedResult_entry)
