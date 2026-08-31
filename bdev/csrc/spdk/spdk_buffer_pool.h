// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lmcache::spdk {

/** Fixed-size SPDK buffers confined to one owning SPDK I/O thread. */
class SpdkBufferPool final {
 public:
  /** Create an empty pool; initialize() allocates after SPDK starts. */
  SpdkBufferPool() = default;

  /** Assert that owner-thread cleanup released every SPDK allocation. */
  ~SpdkBufferPool();

  SpdkBufferPool(const SpdkBufferPool&) = delete;
  SpdkBufferPool& operator=(const SpdkBufferPool&) = delete;

  /**
   * Allocate equally sized buffers using spdk_dma_zmalloc().
   *
   * This is the one implementation detail that makes the otherwise neutrally
   * named SPDK buffers DMA-capable and compliant with bdev alignment.
   */
  void initialize(std::uint64_t buffer_size, std::size_t buffer_alignment,
                  std::size_t buffer_count);

  /** Free all buffers; call only from this pool's owning SPDK thread. */
  void release_all() noexcept;

  /** Lease one buffer index, or nullopt when every buffer is busy. */
  std::optional<std::size_t> try_acquire();

  /** Return the number of buffers currently available for another I/O. */
  std::size_t available() const noexcept;

  /** Return the address associated with a currently leased index. */
  void* data(std::size_t buffer_index);

  /** Return one leased index to the free list. */
  void release(std::size_t buffer_index);

 private:
  std::uint64_t buffer_size_ = 0;
  std::size_t buffer_alignment_ = 0;

  // An index identifies the same allocation in all three tables.
  std::vector<void*> buffers_;
  std::vector<std::uint8_t> in_use_;
  std::vector<std::size_t> free_indexes_;
  bool initialized_ = false;
};

}  // namespace lmcache::spdk
