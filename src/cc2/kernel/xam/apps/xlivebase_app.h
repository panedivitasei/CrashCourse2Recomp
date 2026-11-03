#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>

namespace cc2 {
namespace kernel {
namespace xam {
namespace apps {

using rex::X_HRESULT;

// Replaces the SDK's InitializeKernel (via RuntimeConfig::kernel_init) to register our XLiveBaseApp as 0xFC.
// It can't be wrapped: RegisterApp asserts on the SDK's own 0xFC and keeps the first registration.
void InitializeTitleKernel(rex::Runtime* runtime, rex::system::KernelState* kernel_state);

class XLiveBaseApp : public rex::system::xam::App {
 public:
  explicit XLiveBaseApp(rex::system::KernelState* kernel_state);

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;

 private:
  // CXLiveFriends::Enumerate (msg 0x58020). Reads the marshalled
  // X_CREATE_FRIENDS_ENUMERATOR at buffer_length and returns a valid (empty)
  // enumerator handle so the title's friends/presence init completes.
  X_HRESULT XFriendsCreateEnumerator(uint32_t buffer_length);

  // XStorageBuildServerPath (msg 0x58035) writes back the server path and its length.
  // Titles retry-spin on failure, so storage init needs a valid path and success here.
  X_HRESULT XStorageBuildServerPath(uint32_t buffer_ptr);

  // XStorageDownloadToMemory (msg 0x50009) / XStorageUploadFromMemory
  // (msg 0x5000B). Title Storage transfers, backed by plain host files under
  // <user_data_root>/xstorage/, keyed by the tail of the server path handed
  // out by XStorageBuildServerPath.
  X_HRESULT XStorageDownloadToMemory(uint32_t buffer_ptr);
  X_HRESULT XStorageUploadFromMemory(uint32_t buffer_ptr);
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace cc2
