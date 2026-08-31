// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
// Shared SPDK configuration, result handling, and Python metric conversion.

#pragma once

#include "spdk_bdev_context.h"
#include "spdk_config.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>

namespace lmcache::spdk::pybind {

std::string MakeCoreMask(const std::vector<int> &cpus);

SpdkConfig MakeConfig(const std::string &json_config,
                      const std::string &bdev_name,
                      const std::vector<int> &reactor_cpus,
                      std::size_t io_thread_count, std::size_t buffer_count,
                      int memory_size_mb, bool no_huge, bool no_pci,
                      std::uint64_t io_segment_bytes);

void RequireSuccess(const std::vector<IoResult> &results,
                    const char *operation);

pybind11::dict IoMetricsDict(const IoMetrics &metrics);

} // namespace lmcache::spdk::pybind
