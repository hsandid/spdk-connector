// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lmcache::spdk {

/** Native configuration assembled by pybind before constructing the connector.
 */
struct SpdkConfig {
  // SPDK application and bdev selection.
  std::string json_config_file;
  std::string bdev_name;
  std::string core_mask = "0x1";
  int memory_size_mb = 128;
  bool no_huge = true;
  bool no_pci = true;

  // Maximum bdev I/O size and DMA buffers owned by each SPDK I/O thread.
  std::uint64_t io_segment_size = 1024 * 1024;
  std::size_t spdk_buffer_count = 2;

  // Logical SPDK I/O threads. Each owns one bdev channel and runs on a
  // different reactor selected by core_mask.
  std::size_t io_thread_count = 1;

  // Ordinary ConnectorBase threads that coordinate logical LMCache requests.
  int connector_worker_count = 4;

  // Admission bounds the number of submitted, not yet drained, key operations.
  std::size_t max_pending_operations = 1024;

  // Optional diagnostics. Empty strings fall back to environment variables.
  std::string print_level;
  std::string log_flags;
  std::string tpoint_group_mask;

  // -1 reads LMCACHE_SPDK_STRUCTURED_EVENTS; 0 disables; 1 enables.
  int structured_events = -1;
};

}  // namespace lmcache::spdk
