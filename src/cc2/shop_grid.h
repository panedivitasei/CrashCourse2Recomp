#pragma once
#include <algorithm>
#include <cstdint>

namespace cc2::display {
struct ShopGrid {
  int total, capacity, columns, first, selected;
  bool Valid() const {
    return total > 0 && total <= 4096 && capacity > 0 && capacity <= 10 &&
           columns > 0 && columns <= capacity && first >= 0 && first < total &&
           selected >= 0 && selected < std::min(capacity, total - first);
  }
  bool Scroll(int rows) {
    if (!Valid()) return false;
    const int last = ((std::max(0, total - capacity) + columns - 1) / columns) * columns;
    const int next = int(std::clamp(int64_t(first) + int64_t(rows) * columns, int64_t(0), int64_t(last)));
    if (next == first) return false;
    selected = std::clamp(first + selected - next, 0, std::min(capacity, total - next) - 1);
    first = next;
    return true;
  }
};
}
