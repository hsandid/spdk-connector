// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lmcache::spdk {

/** Immutable storage and I/O geometry discovered during SPDK startup. */
struct StorageGeometry {
  std::uint64_t capacity_bytes = 0;
  std::uint64_t usable_capacity_bytes = 0;
  std::uint64_t page_size = 0;
  std::uint64_t page_count = 0;
  std::uint64_t io_segment_size = 0;
  std::uint64_t bdev_max_rw_size_bytes = 0;
  std::uint32_t block_size = 0;
  std::uint32_t write_unit_blocks = 0;
  std::size_t buffer_alignment = 0;
  std::size_t spdk_buffer_count = 0;
  std::size_t spdk_reactor_count = 0;
  std::size_t spdk_io_thread_count = 0;
  std::size_t total_spdk_buffer_count = 0;
  std::vector<std::uint32_t> spdk_reactor_cores;
  std::vector<std::uint32_t> spdk_io_cores;
};

/**
 * Derive allocatable pages and an effective command limit from one opened bdev.
 *
 * A storage page is one bdev write unit. Capacity is truncated to complete
 * pages, and the requested segment size is capped by the bdev's maximum
 * read/write length before being validated as page aligned.
 */
StorageGeometry BuildStorageGeometry(std::uint64_t num_blocks,
                                     std::uint32_t block_size,
                                     std::uint32_t write_unit_blocks,
                                     std::size_t buffer_alignment,
                                     std::uint64_t io_segment_size,
                                     std::uint32_t bdev_max_rw_size_blocks,
                                     std::size_t spdk_buffer_count);

}  // namespace lmcache::spdk
