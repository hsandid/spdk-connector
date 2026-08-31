// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "connector_base.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <spdk/nvme.h>
#include <spdk/nvme_kv.h>
}

namespace lmcache_spdk_kv {

struct KvConnection {
  spdk_nvme_qpair* qpair = nullptr;
  uint32_t worker_id = 0;

  KvConnection() = default;
  KvConnection(const KvConnection&) = delete;
  KvConnection& operator=(const KvConnection&) = delete;
  KvConnection(KvConnection&& other) noexcept;
  KvConnection& operator=(KvConnection&& other) noexcept;
  ~KvConnection();
};

class SpdkNvmeKvConnector
    : public lmcache::connector::ConnectorBase<KvConnection> {
 public:
  SpdkNvmeKvConnector(std::string pci_bdf, int num_workers,
                      int hugepage_memory_mb, bool trace_events);
  ~SpdkNvmeKvConnector() override;

  uint64_t submit_batch_get(const std::vector<std::string>& keys,
                            const std::vector<void*>& bufs,
                            const std::vector<size_t>& lens,
                            size_t batch_chunk_num_bytes) override;
  uint64_t submit_batch_set(const std::vector<std::string>& keys,
                            const std::vector<void*>& bufs,
                            const std::vector<size_t>& lens,
                            size_t batch_chunk_num_bytes) override;
  uint64_t submit_batch_exists(const std::vector<std::string>& keys) override;
  uint64_t submit_batch_delete(const std::vector<std::string>& keys) override;
  std::vector<lmcache::connector::Completion> drain_completions() override;

 protected:
  KvConnection create_connection() override;
  void do_single_get(KvConnection& conn, const std::string& key, void* buf,
                     size_t len, size_t chunk_size) override;
  void do_single_set(KvConnection& conn, const std::string& key,
                     const void* buf, size_t len, size_t chunk_size) override;
  bool do_single_exists(KvConnection& conn, const std::string& key) override;
  bool do_single_delete(KvConnection& conn, const std::string& key) override;
  void on_workers_stopped() override;

 private:
  using Key = std::array<uint8_t, 16>;

  struct CommandResult {
    bool done = false;
    bool success = false;
  };

  static bool probe_cb(void* context, const spdk_nvme_transport_id* trid,
                       spdk_nvme_ctrlr_opts* options);
  static void attach_cb(void* context, const spdk_nvme_transport_id* trid,
                        spdk_nvme_ctrlr* controller,
                        const spdk_nvme_ctrlr_opts* options);
  static void complete(void* context, const spdk_nvme_cpl* completion);
  static Key derive_key(const std::string& key);

  void initialize_runtime();
  void wait_for_completion(KvConnection& conn, CommandResult* result,
                           const char* operation,
                           bool require_success = true) const;
  void ensure_value_size(size_t len) const;
  void emit_event(const char* phase, const char* operation,
                  const std::string& key, size_t bytes, bool success,
                  uint32_t worker_id, const char* detail,
                  uint64_t future_id = 0) const;

  std::string pci_bdf_;
  std::string environment_name_;
  spdk_nvme_ctrlr* controller_ = nullptr;
  spdk_nvme_ns* namespace_ = nullptr;
  uint32_t max_value_bytes_ = 0;
  bool trace_events_ = false;
  std::atomic<uint32_t> next_worker_id_{0};
  std::mutex runtime_mu_;
  bool runtime_initialized_ = false;
};

}  // namespace lmcache_spdk_kv
