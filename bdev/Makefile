# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0

PYTHON ?= python3
VENV ?= .venv
VENV_PYTHON := $(VENV)/bin/python
LMCACHE_SOURCE_ROOT ?= $(abspath ../../LMCache)
SPDK_ROOT ?= $(abspath ../../spdk)
SPDK_LIBRARY_PATH := $(SPDK_ROOT)/build/lib:$(SPDK_ROOT)/dpdk/build/lib:$(SPDK_ROOT)/isa-l/.libs:$(SPDK_ROOT)/isa-l-crypto/.libs
NO_GPU_EXT ?=
TEST ?= tests
PYTEST_ARGS ?=
VENV_STAMP := $(VENV)/.lmcache-spdk-connector-venv
BUILD_STAMP := build/.lmcache-spdk-connector-build
INPUT_MANIFEST := build/.lmcache-spdk-connector-inputs
PACKAGE_INPUTS := $(wildcard $(shell git ls-files --cached --others --exclude-standard src csrc)) \
	Makefile pyproject.toml setup.py
SPDK_BUILD_INPUT := $(SPDK_ROOT)/build/lib/pkgconfig/spdk_bdev.pc

.PHONY: help venv build test test-all clean FORCE

help:
	@printf '%s\n' \
	  'make venv                          Create the development virtual environment once' \
	  'make build                         Rebuild only when build inputs changed' \
	  'make test                          Build if needed, then run TEST (defaults to tests)' \
	  'make test TEST=tests/test_config.py::test_uses_spdk_defaults' \
	  '                                  Build, then run one test use case' \
	  'make test-all                      Build, then run the complete test suite' \
	  'make build LMCACHE_SOURCE_ROOT=... SPDK_ROOT=...' \
	  '                                  Override the sibling source roots' \
	  'make clean                         Remove local build artifacts'

venv: $(VENV_STAMP)

$(VENV_STAMP):
	$(PYTHON) -m venv "$(VENV)"
	"$(VENV_PYTHON)" -m pip install --upgrade pip setuptools wheel pybind11 pytest
	@touch "$@"

build: $(BUILD_STAMP)

FORCE:

$(INPUT_MANIFEST): FORCE
	@mkdir -p "$(@D)"
	@tmp="$@.tmp"; { \
		printf '%s\n' "VENV=$(VENV)" "LMCACHE_SOURCE_ROOT=$(LMCACHE_SOURCE_ROOT)" "SPDK_ROOT=$(SPDK_ROOT)" "NO_GPU_EXT=$(NO_GPU_EXT)"; \
		git status --porcelain --untracked-files=all -- Makefile pyproject.toml setup.py src csrc; \
		git -C "$(LMCACHE_SOURCE_ROOT)" status --porcelain --untracked-files=all; \
		git -C "$(SPDK_ROOT)" status --porcelain --untracked-files=all; \
	} > "$$tmp"; \
	if ! cmp -s "$$tmp" "$@"; then mv "$$tmp" "$@"; else rm -f "$$tmp"; fi

$(BUILD_STAMP): $(VENV_STAMP) $(INPUT_MANIFEST) $(PACKAGE_INPUTS) $(SPDK_BUILD_INPUT)
	@test -d "$(LMCACHE_SOURCE_ROOT)" || { printf '%s\n' "LMCache source root not found: $(LMCACHE_SOURCE_ROOT)"; exit 1; }
	@test -d "$(SPDK_ROOT)" || { printf '%s\n' "SPDK source root not found: $(SPDK_ROOT)"; exit 1; }
	@if [ -n "$(NO_GPU_EXT)" ]; then \
		NO_GPU_EXT="$(NO_GPU_EXT)" "$(VENV_PYTHON)" -m pip install "$(LMCACHE_SOURCE_ROOT)"; \
	else \
		"$(VENV_PYTHON)" -m pip install "$(LMCACHE_SOURCE_ROOT)"; \
	fi
	LMCACHE_SOURCE_ROOT="$(LMCACHE_SOURCE_ROOT)" SPDK_ROOT="$(SPDK_ROOT)" \
		"$(VENV_PYTHON)" setup.py build_ext --inplace
	LMCACHE_SOURCE_ROOT="$(LMCACHE_SOURCE_ROOT)" SPDK_ROOT="$(SPDK_ROOT)" \
		"$(VENV_PYTHON)" -m pip install --no-deps --no-build-isolation .
	@touch "$@"

test: build
	LD_LIBRARY_PATH="$(SPDK_LIBRARY_PATH)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" \
		"$(VENV_PYTHON)" -m pytest -q $(TEST) $(PYTEST_ARGS)

test-all: build
	LD_LIBRARY_PATH="$(SPDK_LIBRARY_PATH)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" \
		"$(VENV_PYTHON)" -m pytest -q tests $(PYTEST_ARGS)

clean:
	rm -rf build dist .pytest_cache .ruff_cache .mypy_cache \
		.coverage htmlcov
	rm -rf src/*.egg-info
	rm -f src/lmcache_spdk_connector/_native*.so
