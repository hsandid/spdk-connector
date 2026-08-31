// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
// The package exposes one extension; implementation bindings live separately.

#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace lmcache::spdk::pybind {
void BindSpdkConnector(py::module_ &module);
} // namespace lmcache::spdk::pybind

PYBIND11_MODULE(_native, module) {
  module.doc() = "Native SPDK bdev connector for LMCache";
  lmcache::spdk::pybind::BindSpdkConnector(module);
}
