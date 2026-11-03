#include "extra_component.h"

#include <cstdint>
#include <vector>

#include <rex/logging.h>

namespace rex {
namespace avatars {

namespace {
thread_local std::vector<uint8_t> tls_pending_extra_strb;  // consumed by AppendPendingExtraComponent
}  // namespace

void SetPendingExtraComponentStrb(const uint8_t* data, size_t size) {
  if (!data || !size) {
    tls_pending_extra_strb.clear();
    return;
  }
  tls_pending_extra_strb.assign(data, data + size);
}

int AppendPendingExtraComponent(
    std::vector<std::pair<X_AVATAR_COMPONENT_INFO, std::shared_ptr<Model>>>& source_components,
    ModelLoadOptions model_load_options) {
  if (tls_pending_extra_strb.empty()) {
    return -1;
  }
  const std::vector<uint8_t> strb = std::move(tls_pending_extra_strb);
  tls_pending_extra_strb.clear();
  auto model = Model::Load(strb.data(), strb.size(), model_load_options);
  if (!model) {
    REXKRNL_WARN("[avatar] queued extra STRB component failed to decode ({} bytes)", strb.size());
    return -1;
  }
  // Zero asset id + kNone categories: the serializer keys the neutral shader overrides off the returned index.
  X_AVATAR_COMPONENT_INFO info{};
  source_components.push_back({info, model});
  REXKRNL_INFO("[avatar] appended extra STRB component ({} bytes -> component {})", strb.size(),
               source_components.size() - 1);
  return static_cast<int>(source_components.size() - 1);
}

}  // namespace avatars
}  // namespace rex
