// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
// Exercise the LMCache native connector ABI against one NVMe-KV controller.

#include "native_kv_connector.h"

#include <poll.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Connector = lmcache_spdk_kv::SpdkNvmeKvConnector;
using Completion = lmcache::connector::Completion;

Completion wait_for_completion(Connector& connector, uint64_t future_id) {
  struct pollfd event = {connector.event_fd(), POLLIN, 0};
  while (poll(&event, 1, 10000) > 0) {
    for (Completion& completion : connector.drain_completions()) {
      if (completion.future_id == future_id) {
        return completion;
      }
    }
  }
  throw std::runtime_error("native connector completion timed out");
}

void require_success(const Completion& completion, const char* operation) {
  if (!completion.ok) {
    throw std::runtime_error(std::string(operation) + " failed: " +
                             completion.error);
  }
}

void require_result(const Completion& completion, bool expected,
                    const char* operation) {
  require_success(completion, operation);
  if (completion.result_bytes.size() != 1 ||
      static_cast<bool>(completion.result_bytes[0]) != expected) {
    throw std::runtime_error(std::string(operation) + " returned an unexpected bitmap");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <PCIe-BDF>\n", argv[0]);
    return 2;
  }

  try {
    Connector connector(argv[1], 1, 64, true);
    const std::vector<std::string> keys = {"lmcache-native-kv-smoke"};
    std::vector<uint8_t> written(4096, 0);
    std::vector<uint8_t> loaded(written.size(), 0);
    for (size_t index = 0; index < written.size(); ++index) {
      written[index] = static_cast<uint8_t>(index % 251);
    }

    std::vector<void*> write_buffers = {written.data()};
    std::vector<size_t> lengths = {written.size()};
    require_success(wait_for_completion(
                        connector,
                        connector.submit_batch_set(keys, write_buffers, lengths,
                                                   written.size())),
                    "LMCache native SET");

    require_result(wait_for_completion(connector,
                                       connector.submit_batch_exists(keys)),
                   true, "LMCache native EXISTS after SET");

    std::vector<void*> read_buffers = {loaded.data()};
    require_result(wait_for_completion(
                       connector,
                       connector.submit_batch_get(keys, read_buffers, lengths,
                                                  loaded.size())),
                   true, "LMCache native GET");
    if (loaded != written) {
      throw std::runtime_error("LMCache native GET returned different bytes");
    }

    require_result(wait_for_completion(connector,
                                       connector.submit_batch_delete(keys)),
                   true, "LMCache native DELETE");
    require_result(wait_for_completion(connector,
                                       connector.submit_batch_exists(keys)),
                   false, "LMCache native EXISTS after DELETE");
    connector.close();
    std::printf("LMCache native NVMe-KV smoke test passed (4096 bytes)\n");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "LMCache native NVMe-KV smoke test failed: %s\n",
                 error.what());
    return 1;
  }
}
