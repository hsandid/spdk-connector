// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0

#include "page_extent_allocator.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace lmcache::spdk {

PageExtentAllocator::PageExtentAllocator(const std::uint64_t page_count)
    : total_page_count_(page_count), free_page_count_(page_count) {
  if (page_count == 0) {
    throw std::invalid_argument("page allocator requires at least one page");
  }
  free_extents_.emplace(0, page_count);
}

std::vector<PageExtent> PageExtentAllocator::allocate(
    const std::uint64_t page_count) {
  if (page_count == 0) {
    throw std::invalid_argument("page allocation must not be empty");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (page_count > free_page_count_) {
    throw std::runtime_error("page allocator capacity exhausted");
  }

  std::vector<PageExtent> result;
  std::uint64_t remaining = page_count;
  // Prefer lower page numbers, but consume subsequent free ranges when no
  // single range is large enough. The caller owns every returned fragment.
  for (const auto& [first_page, count] : free_extents_) {
    const std::uint64_t allocated = std::min(remaining, count);
    result.push_back({first_page, allocated});
    remaining -= allocated;
    if (remaining == 0) {
      break;
    }
  }

  // Capacity was checked above, so the first-fit scan must have satisfied it.
  if (remaining != 0) {
    throw std::logic_error("page allocator free extent accounting is corrupt");
  }
  for (const PageExtent& extent : result) {
    auto found = free_extents_.find(extent.first_page);
    if (found == free_extents_.end() || found->second < extent.page_count) {
      throw std::logic_error("page allocator free extent accounting is corrupt");
    }
    const std::uint64_t old_count = found->second;
    free_extents_.erase(found);
    if (old_count != extent.page_count) {
      free_extents_.emplace(extent.first_page + extent.page_count,
                            old_count - extent.page_count);
    }
  }
  free_page_count_ -= page_count;
  return result;
}

void PageExtentAllocator::release(const std::vector<PageExtent>& extents) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const PageExtent& extent : extents) {
    if (extent.page_count == 0 ||
        extent.first_page > std::numeric_limits<std::uint64_t>::max() -
                                extent.page_count ||
        extent.first_page + extent.page_count > total_page_count_) {
      throw std::invalid_argument("invalid page extent release");
    }
    // Insert validates against the free list before changing it, which catches
    // double release and overlapping ownership bugs at their source.
    insert_free_extent_locked(extent);
    if (free_page_count_ > std::numeric_limits<std::uint64_t>::max() -
                               extent.page_count) {
      throw std::overflow_error("page allocator free count overflows uint64_t");
    }
    free_page_count_ += extent.page_count;
  }
}

std::uint64_t PageExtentAllocator::free_page_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return free_page_count_;
}

void PageExtentAllocator::insert_free_extent_locked(PageExtent extent) {
  // The ordered map stays disjoint and maximally coalesced. Only the immediate
  // neighbors can overlap or touch a newly returned extent.
  auto next = free_extents_.lower_bound(extent.first_page);
  auto previous = free_extents_.end();
  bool merge_previous = false;
  if (next != free_extents_.begin()) {
    previous = std::prev(next);
    const std::uint64_t previous_end = previous->first + previous->second;
    if (previous_end > extent.first_page) {
      throw std::invalid_argument("page extent release overlaps free space");
    }
    merge_previous = previous_end == extent.first_page;
  }
  const std::uint64_t extent_end = extent.first_page + extent.page_count;
  bool merge_next = false;
  if (next != free_extents_.end()) {
    if (extent_end > next->first) {
      throw std::invalid_argument("page extent release overlaps free space");
    }
    merge_next = extent_end == next->first;
  }
  if (merge_previous) {
    extent.first_page = previous->first;
    extent.page_count += previous->second;
    free_extents_.erase(previous);
  }
  if (merge_next) {
    extent.page_count += next->second;
    free_extents_.erase(next);
  }
  free_extents_.emplace(extent.first_page, extent.page_count);
}

}  // namespace lmcache::spdk
