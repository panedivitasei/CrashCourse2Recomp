#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace cc2::gpu {

struct alignas(64) FloatConstantBlock {
  std::array<uint32_t, 256 * 4> words;
};

// Capture compares every byte before sharing a block; published blocks stay immutable until all readers retire.
class FloatConstantSnapshots {
 public:
  std::shared_ptr<const FloatConstantBlock> Capture(size_t stage, const void* source) {
    auto& previous = previous_.at(stage);
    if (previous && std::memcmp(previous->words.data(), source, sizeof(previous->words)) == 0) {
      return previous;
    }
    auto block = Allocate();
    std::memcpy(block->words.data(), source, sizeof(block->words));
    previous = std::move(block);
    return previous;
  }

 private:
  static std::shared_ptr<FloatConstantBlock> Allocate() {
    struct Chunk { std::array<FloatConstantBlock, 16> items; };
    thread_local std::vector<std::shared_ptr<Chunk>> pool;
    thread_local std::shared_ptr<Chunk> chunk;
    thread_local size_t next = 16, cursor = 0;
    if (next == 16) {
      chunk.reset();
      for (size_t checked = 0; checked < pool.size(); ++checked) {
        cursor = (cursor + 1) % pool.size();
        if (pool[cursor].use_count() == 1) { chunk = pool[cursor]; break; }
      }
      if (!chunk) {
        chunk = std::make_shared<Chunk>();
        if (pool.size() < 128) pool.push_back(chunk);
      }
      next = 0;
    }
    return std::shared_ptr<FloatConstantBlock>(chunk, &chunk->items[next++]);
  }

  std::array<std::shared_ptr<const FloatConstantBlock>, 2> previous_;
};

}  // namespace cc2::gpu
