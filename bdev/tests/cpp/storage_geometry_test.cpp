// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0

#include "storage_geometry.h"

#include <cassert>
#include <limits>
#include <stdexcept>

namespace {

template <typename Exception, typename Operation>
void ExpectException(Operation operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const Exception&) {
    rejected = true;
  }
  assert(rejected);
}

}  // namespace

int main() {
  const auto unbounded = lmcache::spdk::BuildStorageGeometry(
      1024, 4096, 1, 4096, 1024 * 1024, 0, 16);
  assert(unbounded.io_segment_size == 1024 * 1024);
  assert(unbounded.bdev_max_rw_size_bytes == 0);

  const auto capped = lmcache::spdk::BuildStorageGeometry(
      1024, 4096, 1, 4096, 1024 * 1024, 128, 16);
  assert(capped.io_segment_size == 128 * 4096);
  assert(capped.bdev_max_rw_size_bytes == 128 * 4096);

  const auto rounded = lmcache::spdk::BuildStorageGeometry(
      1024, 4096, 2, 4096, 1024 * 1024, 129, 16);
  assert(rounded.io_segment_size == 128 * 4096);
  assert(rounded.bdev_max_rw_size_bytes == 129 * 4096);

  ExpectException<std::invalid_argument>([] {
    (void)lmcache::spdk::BuildStorageGeometry(1024, 4096, 2, 4096,
                                               1024 * 1024, 1, 16);
  });

  const auto truncated_capacity = lmcache::spdk::BuildStorageGeometry(
      5, 4096, 2, 4096, 8192, 0, 1);
  assert(truncated_capacity.capacity_bytes == 5 * 4096);
  assert(truncated_capacity.page_size == 8192);
  assert(truncated_capacity.page_count == 2);
  assert(truncated_capacity.usable_capacity_bytes == 4 * 4096);

  ExpectException<std::invalid_argument>([] {
    (void)lmcache::spdk::BuildStorageGeometry(1024, 4096, 2, 4096, 4096, 0,
                                               1);
  });
  ExpectException<std::invalid_argument>([] {
    (void)lmcache::spdk::BuildStorageGeometry(1, 4096, 2, 4096, 8192, 0, 1);
  });
  ExpectException<std::invalid_argument>([] {
    (void)lmcache::spdk::BuildStorageGeometry(1024, 4096, 1, 3, 4096, 0, 1);
  });
  ExpectException<std::invalid_argument>([] {
    (void)lmcache::spdk::BuildStorageGeometry(1024, 4096, 1, 4096, 4096, 0,
                                               0);
  });
  ExpectException<std::overflow_error>([] {
    (void)lmcache::spdk::BuildStorageGeometry(
        std::numeric_limits<std::uint64_t>::max(), 2, 1, 4096, 4096, 0, 1);
  });
  return 0;
}
