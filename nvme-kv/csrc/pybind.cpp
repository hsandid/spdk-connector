// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
#include "native_kv_connector.h"

#include "connector_pybind_utils.h"

#include <pybind11/pybind11.h>

namespace py = pybind11;

PYBIND11_MODULE(_native, module) {
  module.doc() = "Direct SPDK NVMe-KV native connector for LMCache";

  py::class_<lmcache_spdk_kv::SpdkNvmeKvConnector>(module,
                                                    "SpdkNvmeKvConnector")
      .def(py::init<std::string, int, int, bool>(), py::arg("pci_bdf"),
           py::arg("num_workers") = 1,
           py::arg("hugepage_memory_mb") = 64,
           py::arg("trace_events") = false)
      LMCACHE_BIND_CONNECTOR_METHODS(lmcache_spdk_kv::SpdkNvmeKvConnector);
}
