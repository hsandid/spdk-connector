// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0

#include "spdk_pybind_common.h"

#include <algorithm>
#include <stdexcept>

namespace py = pybind11;

namespace lmcache::spdk::pybind {

std::string MakeCoreMask(const std::vector<int> &cpus) {
  if (cpus.empty()) {
    throw std::invalid_argument("reactor_cpus must not be empty");
  }
  int highest_cpu = 0;
  for (const int cpu : cpus) {
    if (cpu < 0) {
      throw std::invalid_argument("SPDK reactor CPU must be non-negative");
    }
    highest_cpu = std::max(highest_cpu, cpu);
  }
  std::string digits(static_cast<std::size_t>(highest_cpu / 4 + 1), '0');
  for (const int cpu : cpus) {
    const std::size_t digit =
        digits.size() - 1 - static_cast<std::size_t>(cpu / 4);
    const unsigned bit = static_cast<unsigned>(cpu % 4);
    const unsigned value =
        digits[digit] <= '9' ? static_cast<unsigned>(digits[digit] - '0')
                             : static_cast<unsigned>(digits[digit] - 'a' + 10);
    const unsigned updated = value | (1U << bit);
    digits[digit] = updated < 10 ? static_cast<char>('0' + updated)
                                 : static_cast<char>('a' + updated - 10);
  }
  return "0x" + digits;
}

SpdkConfig MakeConfig(const std::string &json_config,
                      const std::string &bdev_name,
                      const std::vector<int> &reactor_cpus,
                      const std::size_t io_thread_count,
                      const std::size_t buffer_count, const int memory_size_mb,
                      const bool no_huge, const bool no_pci,
                      const std::uint64_t io_segment_bytes) {
  SpdkConfig config;
  config.json_config_file = json_config;
  config.bdev_name = bdev_name;
  config.core_mask = MakeCoreMask(reactor_cpus);
  config.memory_size_mb = memory_size_mb;
  config.no_huge = no_huge;
  config.no_pci = no_pci;
  config.io_segment_size = io_segment_bytes;
  config.spdk_buffer_count = buffer_count;
  config.io_thread_count = io_thread_count;
  return config;
}

void RequireSuccess(const std::vector<IoResult> &results,
                    const char *operation) {
  for (const IoResult &result : results) {
    if (!result.success) {
      throw std::runtime_error(std::string(operation) +
                               " failed: " + result.error);
    }
  }
}

py::dict IoMetricsDict(const IoMetrics &metrics) {
  py::dict result;
  result["physical_io_submitted"] = metrics.submitted;
  result["physical_io_completed"] = metrics.completed;
  result["physical_io_succeeded"] = metrics.succeeded;
  result["physical_io_failed"] = metrics.failed;
  result["worker_queue_wait_ns"] = metrics.worker_queue_wait_ns;
  result["dma_buffer_pool_starvation_count"] = metrics.buffer_pool_starvations;
  result["dma_buffer_pool_starvation_wait_ns"] = metrics.buffer_pool_wait_ns;
  result["spdk_enomem_queue_retry_count"] = metrics.queue_retries;
  result["spdk_enomem_queue_retry_wait_ns"] = metrics.queue_retry_wait_ns;
  result["dma_buffer_pool_allocated_bytes"] =
      metrics.dma_buffer_pool_allocated_bytes;
  result["dma_buffer_pool_in_use_bytes"] = metrics.dma_buffer_pool_in_use_bytes;
  result["dma_buffer_pool_peak_in_use_bytes"] =
      metrics.dma_buffer_pool_peak_in_use_bytes;
  result["submitted_logical_bytes"] = metrics.submitted_logical_bytes;
  result["submitted_physical_bytes"] = metrics.submitted_physical_bytes;
  result["submitted_unused_dma_buffer_bytes"] =
      metrics.submitted_unused_dma_buffer_bytes;
  result["submitted_write_padding_bytes"] =
      metrics.submitted_write_padding_bytes;
  return result;
}

} // namespace lmcache::spdk::pybind
