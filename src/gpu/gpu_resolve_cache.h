#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gpu/gpu_records.h"

namespace cc2::gpu::tracker {

// Callers serialize the table; the epoch lets draw caches notice retired address aliases.
class ResolveTextureCache {
 public:
  ResourceDesc Find(uint32_t base) const {
    const auto it = entries_.find(base);
    return it == entries_.end() ? ResourceDesc{} : it->second.desc;
  }

  ResourceDesc Find(uint32_t base, uint32_t width, uint32_t height, uint32_t guest_format) const {
    const auto it = entries_.find(base);
    if (it == entries_.end()) return {};
    const auto& entry = it->second;
    return entry.desc.width == width && entry.desc.height == height && entry.guest_format == guest_format
        ? entry.desc : ResourceDesc{};
  }

  void Store(uint32_t base, uint32_t size, const ResourceDesc& desc, uint32_t guest_format) {
    if (const auto it = entries_.find(base); it != entries_.end() && it->second.desc.id != desc.id) {
      retired_.push_back(it->second.desc.id);
    }
    entries_[base] = {desc, size, guest_format};
    epoch_.fetch_add(1, std::memory_order_release);
  }

  uint32_t Invalidate(uint32_t base, uint32_t size) {
    if (!size) return 0;
    uint32_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (uint64_t(base) < uint64_t(it->first) + it->second.size &&
          uint64_t(it->first) < uint64_t(base) + size) {
        retired_.push_back(it->second.desc.id);
        it = entries_.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
    if (removed) epoch_.fetch_add(1, std::memory_order_release);
    return removed;
  }

  uint32_t Epoch() const { return epoch_.load(std::memory_order_acquire); }

  // Ids of entries dropped since the last call.
  std::vector<uint32_t> TakeRetired() { return std::exchange(retired_, {}); }

 private:
  struct Entry {
    ResourceDesc desc;
    uint32_t size;
    uint32_t guest_format;
  };
  std::unordered_map<uint32_t, Entry> entries_;
  std::vector<uint32_t> retired_;
  std::atomic<uint32_t> epoch_{1};
};

}  // namespace cc2::gpu::tracker
