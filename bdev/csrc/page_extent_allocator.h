// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
// Ephemeral page allocator for the external native SPDK connector.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace lmcache::spdk {

struct PageExtent {
  std::uint64_t first_page = 0;
  std::uint64_t page_count = 0;
};

/**
 * Thread-safe first-fit allocator over an in-memory page address space.
 *
 * Allocation metadata deliberately has no on-device representation: external
 * native connector placement is valid only for this process lifetime.
 */
class PageExtentAllocator final {
 public:
  explicit PageExtentAllocator(std::uint64_t page_count);

  PageExtentAllocator(const PageExtentAllocator&) = delete;
  PageExtentAllocator& operator=(const PageExtentAllocator&) = delete;

  /** Reserve exactly page_count pages, possibly from multiple free extents. */
  std::vector<PageExtent> allocate(std::uint64_t page_count);

  /**
   * Return one allocation exactly once to the free address space.
   *
   * Every extent must be in range and must not overlap existing free space.
   * The allocator holds its mutex while validating and merging adjacent free
   * extents, so callers may release fragmented allocations concurrently.
   */
  void release(const std::vector<PageExtent>& extents);

  std::uint64_t free_page_count() const;

 private:
  void insert_free_extent_locked(PageExtent extent);

  mutable std::mutex mutex_;
  std::map<std::uint64_t, std::uint64_t> free_extents_;
  std::uint64_t total_page_count_ = 0;
  std::uint64_t free_page_count_ = 0;
};

}  // namespace lmcache::spdk
