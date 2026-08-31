// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0

#include "spdk_buffer_pool.h"

#include <spdk/env.h>

#include <limits>
#include <stdexcept>
#include <string>

namespace lmcache::spdk {
namespace {

bool IsPowerOfTwo(std::size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

}  // namespace

SpdkBufferPool::~SpdkBufferPool() {
  // SPDK memory must be released on its owning thread before this ordinary C++
  // destructor runs. Terminating makes a lifecycle regression visible.
  if (!buffers_.empty()) {
    std::terminate();
  }
}

void SpdkBufferPool::initialize(std::uint64_t buffer_size,
                                std::size_t buffer_alignment,
                                std::size_t buffer_count) {
  if (initialized_ || !buffers_.empty()) {
    throw std::logic_error("SPDK buffer pool is already initialized");
  }
  if (buffer_size == 0) {
    throw std::invalid_argument("SPDK buffer size must be greater than zero");
  }
  if (buffer_size > std::numeric_limits<std::size_t>::max()) {
    throw std::overflow_error("SPDK buffer size does not fit in size_t");
  }
  if (!IsPowerOfTwo(buffer_alignment)) {
    throw std::invalid_argument(
        "SPDK buffer alignment must be a nonzero power of two");
  }
  if (buffer_count == 0) {
    throw std::invalid_argument("SPDK buffer count must be greater than zero");
  }
  if (buffer_count > std::numeric_limits<std::size_t>::max() /
                         static_cast<std::size_t>(buffer_size)) {
    throw std::overflow_error("total SPDK buffer pool size overflows size_t");
  }

  buffer_size_ = buffer_size;
  buffer_alignment_ = buffer_alignment;
  buffers_.reserve(buffer_count);
  in_use_.assign(buffer_count, 0);
  free_indexes_.reserve(buffer_count);

  for (std::size_t index = 0; index < buffer_count; ++index) {
    void* buffer = spdk_dma_zmalloc(static_cast<std::size_t>(buffer_size),
                                    buffer_alignment, nullptr);
    if (buffer == nullptr) {
      const std::size_t allocated = buffers_.size();
      release_all();
      throw std::runtime_error(
          "spdk_dma_zmalloc failed after allocating " +
          std::to_string(allocated) + " of " + std::to_string(buffer_count) +
          " SPDK buffers; partial allocations were released");
    }
    buffers_.push_back(buffer);
  }

  for (std::size_t index = buffer_count; index > 0; --index) {
    free_indexes_.push_back(index - 1);
  }
  initialized_ = true;
}

void SpdkBufferPool::release_all() noexcept {
  for (void* buffer : buffers_) {
    spdk_dma_free(buffer);
  }
  buffers_.clear();
  in_use_.clear();
  free_indexes_.clear();
  buffer_size_ = 0;
  buffer_alignment_ = 0;
  initialized_ = false;
}

std::optional<std::size_t> SpdkBufferPool::try_acquire() {
  if (!initialized_) {
    throw std::logic_error("SPDK buffer pool is not initialized");
  }
  if (free_indexes_.empty()) {
    return std::nullopt;
  }

  const std::size_t index = free_indexes_.back();
  free_indexes_.pop_back();
  in_use_.at(index) = 1;
  return index;
}

std::size_t SpdkBufferPool::available() const noexcept {
  return free_indexes_.size();
}

void* SpdkBufferPool::data(std::size_t buffer_index) {
  if (!initialized_) {
    throw std::logic_error("SPDK buffer pool is not initialized");
  }
  if (buffer_index >= buffers_.size() || in_use_.at(buffer_index) == 0) {
    throw std::invalid_argument("SPDK buffer index is not currently leased");
  }
  return buffers_.at(buffer_index);
}

void SpdkBufferPool::release(std::size_t buffer_index) {
  if (!initialized_) {
    throw std::logic_error("SPDK buffer pool is not initialized");
  }
  if (buffer_index >= buffers_.size() || in_use_.at(buffer_index) == 0) {
    throw std::invalid_argument("SPDK buffer index is not currently leased");
  }
  in_use_[buffer_index] = 0;
  free_indexes_.push_back(buffer_index);
}

}  // namespace lmcache::spdk
