// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
// Storage geometry is shared by the direct and native page-extent allocators.

#include "storage_geometry.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lmcache::spdk {
namespace {

std::uint64_t CheckedMultiply(const std::uint64_t left,
                              const std::uint64_t right,
                              const char* description) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    throw std::overflow_error(description);
  }
  return left * right;
}

bool IsPowerOfTwo(const std::size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

}  // namespace

StorageGeometry BuildStorageGeometry(const std::uint64_t num_blocks,
                                     const std::uint32_t block_size,
                                      const std::uint32_t write_unit_blocks,
                                      const std::size_t buffer_alignment,
                                      const std::uint64_t io_segment_size,
                                      const std::uint32_t bdev_max_rw_size_blocks,
                                      const std::size_t spdk_buffer_count) {
  if (num_blocks == 0 || block_size == 0 || write_unit_blocks == 0) {
    throw std::invalid_argument("bdev geometry contains a zero-sized dimension");
  }
  if (!IsPowerOfTwo(buffer_alignment)) {
    throw std::invalid_argument("bdev buffer alignment must be a power of two");
  }
  if (spdk_buffer_count == 0) {
    throw std::invalid_argument("spdk_buffer_count must be greater than zero");
  }

  StorageGeometry geometry;
  geometry.capacity_bytes = CheckedMultiply(
      num_blocks, block_size, "bdev byte capacity overflows uint64_t");
  geometry.page_size = CheckedMultiply(
      block_size, write_unit_blocks, "bdev write unit overflows uint64_t");
  // SPDK reports its transfer ceiling in blocks. Our allocator and write path
  // operate in whole write-unit pages, so round that ceiling down before using
  // it to cap the caller's requested tuning value.
  const std::uint64_t bdev_max_rw_size_bytes = bdev_max_rw_size_blocks == 0
      ? 0
      : CheckedMultiply(bdev_max_rw_size_blocks, block_size,
                        "bdev maximum I/O size overflows uint64_t");
  const std::uint64_t bdev_max_page_aligned_bytes =
      bdev_max_rw_size_bytes / geometry.page_size * geometry.page_size;
  const std::uint64_t effective_io_segment_size =
      bdev_max_rw_size_bytes == 0
          ? io_segment_size
          : std::min(io_segment_size, bdev_max_page_aligned_bytes);
  if (effective_io_segment_size < geometry.page_size ||
      effective_io_segment_size % geometry.page_size != 0 ||
      effective_io_segment_size > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument(
        "effective SPDK I/O segment size must be a storage-page-aligned bdev command size");
  }
  // A partial final write unit is not safely writable, so it is deliberately
  // excluded from the page allocator rather than exposed as usable capacity.
  geometry.page_count = geometry.capacity_bytes / geometry.page_size;
  if (geometry.page_count == 0) {
    throw std::invalid_argument("bdev has no complete storage pages");
  }
  geometry.usable_capacity_bytes = CheckedMultiply(
      geometry.page_count, geometry.page_size,
      "usable bdev capacity overflows uint64_t");
  geometry.io_segment_size = effective_io_segment_size;
  geometry.bdev_max_rw_size_bytes = bdev_max_rw_size_bytes;
  geometry.block_size = block_size;
  geometry.write_unit_blocks = write_unit_blocks;
  geometry.buffer_alignment = buffer_alignment;
  geometry.spdk_buffer_count = spdk_buffer_count;
  return geometry;
}

}  // namespace lmcache::spdk
