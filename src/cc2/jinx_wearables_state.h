#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace cc2::jinx {
struct WearableRequest {
  uint64_t generation = 0, actor_id = 0;
  uint32_t slot = 0, game = 0, actor = 0, resource = 0;
};
class WearableRequests {
 public:
  WearableRequest Begin(uint32_t slot, uint32_t game, uint32_t actor,
                        uint64_t actor_id, uint32_t resource) {
    const auto generation = ++generation_;
    current_.at(slot) = generation;
    return {generation, actor_id, slot, game, actor, resource};
  }
  void Cancel(uint32_t slot) { current_.at(slot) = 0; }
  bool Current(const WearableRequest& r) const {
    return r.slot < current_.size() && r.generation && current_[r.slot] == r.generation;
  }
  void Bind(uint32_t task, const WearableRequest* request) {
    tasks_.erase(task);
    if (request && task) tasks_.emplace(task, *request);
  }
  std::optional<WearableRequest> Take(uint32_t task) {
    const auto it = tasks_.find(task);
    if (it == tasks_.end()) return std::nullopt;
    const auto r = it->second;
    tasks_.erase(it);
    return r;
  }
  void Destroy(uint32_t task) { tasks_.erase(task); }
 private:
  uint64_t generation_ = 0;
  std::array<uint64_t, 4> current_{};
  std::unordered_map<uint32_t, WearableRequest> tasks_;
};
}  // namespace cc2::jinx
