// XGI app whose XUserReadStats always returns a well-formed result, even with no data.
// NETMANLA::UpdateFriendsLeaderboardsList qsorts rows_ptr by num_rows straight from it, so an unfilled success crashes.

#pragma once

#include <rex/kernel/xam/apps/xgi_app.h>

namespace cc2::kernel::xam::apps {

using rex::X_HRESULT;

class Cc2XgiApp : public rex::kernel::xam::apps::XgiApp {
 public:
  using XgiApp::XgiApp;

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;

 private:
  // XSession create/join/leave/start/end/modify/delete/search for a locally hosted session.
  X_HRESULT DispatchSessionMessage(uint32_t message, uint32_t buffer_ptr, uint32_t buffer_length);
};

}  // namespace cc2::kernel::xam::apps
