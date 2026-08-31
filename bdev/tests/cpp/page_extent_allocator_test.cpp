// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0

#include "page_extent_allocator.h"

#include <cassert>
#include <limits>
#include <stdexcept>
#include <vector>

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
  ExpectException<std::invalid_argument>(
      [] { (void)lmcache::spdk::PageExtentAllocator(0); });

  lmcache::spdk::PageExtentAllocator allocator(10);
  const std::vector<lmcache::spdk::PageExtent> first = allocator.allocate(3);
  const std::vector<lmcache::spdk::PageExtent> second = allocator.allocate(2);
  assert(first.size() == 1 && first[0].first_page == 0 &&
         first[0].page_count == 3);
  assert(second.size() == 1 && second[0].first_page == 3 &&
         second[0].page_count == 2);

  allocator.release(first);
  const std::vector<lmcache::spdk::PageExtent> fragmented = allocator.allocate(4);
  assert(fragmented.size() == 2);
  assert(fragmented[0].first_page == 0 && fragmented[0].page_count == 3);
  assert(fragmented[1].first_page == 5 && fragmented[1].page_count == 1);
  assert(allocator.free_page_count() == 4);

  allocator.release(second);
  allocator.release(fragmented);
  assert(allocator.free_page_count() == 10);

  const std::vector<lmcache::spdk::PageExtent> full = allocator.allocate(10);
  assert(full.size() == 1 && full[0].first_page == 0 && full[0].page_count == 10);
  assert(allocator.free_page_count() == 0);
  allocator.release(full);

  ExpectException<std::invalid_argument>([&] { (void)allocator.allocate(0); });
  ExpectException<std::runtime_error>([&] { (void)allocator.allocate(11); });
  ExpectException<std::invalid_argument>([&] { allocator.release({{0, 1}}); });
  ExpectException<std::invalid_argument>([&] { allocator.release({{10, 1}}); });
  ExpectException<std::invalid_argument>(
      [&] { allocator.release({{std::numeric_limits<std::uint64_t>::max(), 1}}); });
  assert(allocator.free_page_count() == 10);

  lmcache::spdk::PageExtentAllocator coalescing(6);
  const std::vector<lmcache::spdk::PageExtent> left = coalescing.allocate(2);
  const std::vector<lmcache::spdk::PageExtent> middle = coalescing.allocate(2);
  const std::vector<lmcache::spdk::PageExtent> right = coalescing.allocate(2);
  coalescing.release(left);
  coalescing.release(right);
  coalescing.release(middle);
  const std::vector<lmcache::spdk::PageExtent> merged = coalescing.allocate(6);
  assert(merged.size() == 1 && merged[0].first_page == 0 &&
         merged[0].page_count == 6);
  return 0;
}
