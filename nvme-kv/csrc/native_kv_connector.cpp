// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
#include "native_kv_connector.h"

#include <openssl/sha.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace lmcache_spdk_kv {
namespace {

constexpr char kKeyDomain[] = "lmcache-spdk-nvme-kv/native-l2/v1\0";
constexpr auto kCommandTimeout = std::chrono::seconds(10);

std::mutex g_runtime_mu;
bool g_runtime_active = false;

class DmaBuffer {
 public:
  explicit DmaBuffer(size_t size)
      : data_(spdk_dma_zmalloc(size, 4096, nullptr)) {
    if (data_ == nullptr) {
      throw std::runtime_error("SPDK DMA allocation failed");
    }
  }

  ~DmaBuffer() { spdk_dma_free(data_); }

  DmaBuffer(const DmaBuffer&) = delete;
  DmaBuffer& operator=(const DmaBuffer&) = delete;

  void* data() { return data_; }
  const void* data() const { return data_; }

 private:
  void* data_;
};

}  // namespace

KvConnection::KvConnection(KvConnection&& other) noexcept
    : qpair(other.qpair), worker_id(other.worker_id) {
  other.qpair = nullptr;
}

KvConnection& KvConnection::operator=(KvConnection&& other) noexcept {
  if (this != &other) {
    if (qpair != nullptr) {
      spdk_nvme_ctrlr_free_io_qpair(qpair);
    }
    qpair = other.qpair;
    worker_id = other.worker_id;
    other.qpair = nullptr;
  }
  return *this;
}

KvConnection::~KvConnection() {
  if (qpair != nullptr) {
    spdk_nvme_ctrlr_free_io_qpair(qpair);
  }
}

SpdkNvmeKvConnector::SpdkNvmeKvConnector(std::string pci_bdf, int num_workers,
                                         int hugepage_memory_mb,
                                         bool trace_events)
    : ConnectorBase(num_workers),
      pci_bdf_(std::move(pci_bdf)),
      environment_name_("lmcache_spdk_kv_native"),
      trace_events_(trace_events) {
  if (pci_bdf_.empty()) {
    throw std::runtime_error("pci_bdf must not be empty");
  }
  if (hugepage_memory_mb <= 0) {
    throw std::runtime_error("hugepage_memory_mb must be positive");
  }

  struct spdk_env_opts options = {};
  options.opts_size = sizeof(options);
  spdk_env_opts_init(&options);
  options.name = environment_name_.c_str();
  options.mem_size = hugepage_memory_mb;

  std::lock_guard<std::mutex> runtime_lock(g_runtime_mu);
  if (g_runtime_active) {
    throw std::runtime_error("only one NVMe-KV SPDK connector may run per process");
  }
  if (spdk_env_init(&options) < 0) {
    throw std::runtime_error("SPDK environment initialization failed");
  }
  g_runtime_active = true;
  runtime_initialized_ = true;

  try {
    initialize_runtime();
    emit_event("ready", "attach", pci_bdf_, max_value_bytes_, true, 0, "kv_namespace");
    start_workers();
  } catch (...) {
    if (controller_ != nullptr) {
      spdk_nvme_detach(controller_);
      controller_ = nullptr;
    }
    spdk_env_fini();
    runtime_initialized_ = false;
    g_runtime_active = false;
    throw;
  }
}

SpdkNvmeKvConnector::~SpdkNvmeKvConnector() { close(); }

uint64_t SpdkNvmeKvConnector::submit_batch_get(
    const std::vector<std::string>& keys, const std::vector<void*>& bufs,
    const std::vector<size_t>& lens, size_t batch_chunk_num_bytes) {
  const uint64_t future_id = ConnectorBase::submit_batch_get(
      keys, bufs, lens, batch_chunk_num_bytes);
  emit_event("submit", "get", "", batch_chunk_num_bytes, true, 0, "batch",
             future_id);
  return future_id;
}

uint64_t SpdkNvmeKvConnector::submit_batch_set(
    const std::vector<std::string>& keys, const std::vector<void*>& bufs,
    const std::vector<size_t>& lens, size_t batch_chunk_num_bytes) {
  const uint64_t future_id = ConnectorBase::submit_batch_set(
      keys, bufs, lens, batch_chunk_num_bytes);
  emit_event("submit", "set", "", batch_chunk_num_bytes, true, 0, "batch",
             future_id);
  return future_id;
}

uint64_t SpdkNvmeKvConnector::submit_batch_exists(
    const std::vector<std::string>& keys) {
  const uint64_t future_id = ConnectorBase::submit_batch_exists(keys);
  emit_event("submit", "exists", "", 0, true, 0, "batch", future_id);
  return future_id;
}

uint64_t SpdkNvmeKvConnector::submit_batch_delete(
    const std::vector<std::string>& keys) {
  const uint64_t future_id = ConnectorBase::submit_batch_delete(keys);
  emit_event("submit", "delete", "", 0, true, 0, "batch", future_id);
  return future_id;
}

std::vector<lmcache::connector::Completion>
SpdkNvmeKvConnector::drain_completions() {
  auto completions = ConnectorBase::drain_completions();
  for (const auto& completion : completions) {
    emit_event("complete", "batch", "", 0, completion.ok, 0,
               completion.error.empty() ? "ok" : "error", completion.future_id);
  }
  return completions;
}

KvConnection SpdkNvmeKvConnector::create_connection() {
  KvConnection connection;
  connection.qpair = spdk_nvme_ctrlr_alloc_io_qpair(controller_, nullptr, 0);
  if (connection.qpair == nullptr) {
    throw std::runtime_error("SPDK qpair allocation failed");
  }
  connection.worker_id = next_worker_id_.fetch_add(1, std::memory_order_relaxed);
  return connection;
}

void SpdkNvmeKvConnector::do_single_get(KvConnection& conn,
                                        const std::string& key, void* buf,
                                        size_t len, size_t chunk_size) {
  (void)chunk_size;
  ensure_value_size(len);
  const Key device_key = derive_key(key);
  DmaBuffer staging(len);
  CommandResult result;
  emit_event("io_begin", "retrieve", key, len, true, conn.worker_id, "");
  const int rc = spdk_nvme_kv_retrieve(namespace_, conn.qpair, device_key.data(),
                                       device_key.size(), staging.data(), len,
                                       complete, &result, 0);
  if (rc != 0) {
    emit_event("io_end", "retrieve", key, len, false, conn.worker_id, "submit_failed");
    throw std::runtime_error("NVMe-KV retrieve submission failed");
  }
  wait_for_completion(conn, &result, "retrieve");
  std::memcpy(buf, staging.data(), len);
  emit_event("io_end", "retrieve", key, len, true, conn.worker_id, "ok");
}

void SpdkNvmeKvConnector::do_single_set(KvConnection& conn,
                                        const std::string& key,
                                        const void* buf, size_t len,
                                        size_t chunk_size) {
  (void)chunk_size;
  ensure_value_size(len);
  const Key device_key = derive_key(key);
  DmaBuffer staging(len);
  std::memcpy(staging.data(), buf, len);
  CommandResult result;
  emit_event("io_begin", "store", key, len, true, conn.worker_id, "");
  const int rc = spdk_nvme_kv_store(namespace_, conn.qpair, device_key.data(),
                                    device_key.size(), staging.data(), len,
                                    complete, &result, 0);
  if (rc != 0) {
    emit_event("io_end", "store", key, len, false, conn.worker_id, "submit_failed");
    throw std::runtime_error("NVMe-KV store submission failed");
  }
  wait_for_completion(conn, &result, "store");
  emit_event("io_end", "store", key, len, true, conn.worker_id, "ok");
}

bool SpdkNvmeKvConnector::do_single_exists(KvConnection& conn,
                                            const std::string& key) {
  const Key device_key = derive_key(key);
  CommandResult result;
  const int rc = spdk_nvme_kv_exist(namespace_, conn.qpair, device_key.data(),
                                    device_key.size(), complete, &result);
  if (rc != 0) {
    throw std::runtime_error("NVMe-KV exist submission failed");
  }
  wait_for_completion(conn, &result, "exist", false);
  emit_event("io_end", "exist", key, 0, result.success, conn.worker_id,
             result.success ? "present" : "absent");
  return result.success;
}

bool SpdkNvmeKvConnector::do_single_delete(KvConnection& conn,
                                            const std::string& key) {
  if (!do_single_exists(conn, key)) {
    return false;
  }
  const Key device_key = derive_key(key);
  CommandResult result;
  const int rc = spdk_nvme_kv_delete(namespace_, conn.qpair, device_key.data(),
                                     device_key.size(), complete, &result);
  if (rc != 0) {
    throw std::runtime_error("NVMe-KV delete submission failed");
  }
  wait_for_completion(conn, &result, "delete");
  emit_event("io_end", "delete", key, 0, true, conn.worker_id, "deleted");
  return true;
}

void SpdkNvmeKvConnector::on_workers_stopped() {
  std::lock_guard<std::mutex> lock(runtime_mu_);
  if (!runtime_initialized_) {
    return;
  }
  emit_event("shutdown", "detach", pci_bdf_, 0, true, 0, "");
  if (controller_ != nullptr) {
    spdk_nvme_detach(controller_);
    controller_ = nullptr;
    namespace_ = nullptr;
  }
  spdk_env_fini();
  runtime_initialized_ = false;
  std::lock_guard<std::mutex> global_lock(g_runtime_mu);
  g_runtime_active = false;
}

bool SpdkNvmeKvConnector::probe_cb(void* context,
                                   const spdk_nvme_transport_id* trid,
                                   spdk_nvme_ctrlr_opts* options) {
  (void)trid;
  (void)options;
  const auto* connector = static_cast<SpdkNvmeKvConnector*>(context);
  return connector->controller_ == nullptr;
}

void SpdkNvmeKvConnector::attach_cb(void* context,
                                    const spdk_nvme_transport_id* trid,
                                    spdk_nvme_ctrlr* controller,
                                    const spdk_nvme_ctrlr_opts* options) {
  (void)trid;
  (void)options;
  auto* connector = static_cast<SpdkNvmeKvConnector*>(context);
  for (uint32_t nsid = spdk_nvme_ctrlr_get_first_active_ns(controller); nsid != 0;
       nsid = spdk_nvme_ctrlr_get_next_active_ns(controller, nsid)) {
    spdk_nvme_ns* ns = spdk_nvme_ctrlr_get_ns(controller, nsid);
    if (ns == nullptr || spdk_nvme_ns_get_csi(ns) != SPDK_NVME_CSI_KV) {
      continue;
    }
    const auto* data = spdk_nvme_kv_ns_get_data(ns);
    if (data == nullptr) {
      continue;
    }
    const auto& format = data->kvf[data->kvfc.kvfi];
    if (format.kvkml < 16 || format.kvvml == 0) {
      continue;
    }
    connector->controller_ = controller;
    connector->namespace_ = ns;
    connector->max_value_bytes_ = format.kvvml;
    return;
  }
  spdk_nvme_detach(controller);
}

void SpdkNvmeKvConnector::complete(void* context,
                                   const spdk_nvme_cpl* completion) {
  auto* result = static_cast<CommandResult*>(context);
  result->success = !spdk_nvme_cpl_is_error(completion);
  result->done = true;
}

SpdkNvmeKvConnector::Key SpdkNvmeKvConnector::derive_key(
    const std::string& key) {
  std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{};
  std::string material(kKeyDomain, sizeof(kKeyDomain) - 1);
  material.append(key);
  SHA256(reinterpret_cast<const unsigned char*>(material.data()), material.size(),
         digest.data());
  Key result{};
  std::memcpy(result.data(), digest.data(), result.size());
  return result;
}

void SpdkNvmeKvConnector::initialize_runtime() {
  spdk_nvme_transport_id trid{};
  spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
  std::snprintf(trid.traddr, sizeof(trid.traddr), "%s", pci_bdf_.c_str());
  if (spdk_nvme_probe(&trid, this, probe_cb, attach_cb, nullptr) != 0 ||
      controller_ == nullptr || namespace_ == nullptr) {
    throw std::runtime_error("no NVMe Key-Value namespace attached at " + pci_bdf_);
  }
}

void SpdkNvmeKvConnector::wait_for_completion(KvConnection& conn,
                                              CommandResult* result,
                                              const char* operation,
                                              bool require_success) const {
  const auto deadline = std::chrono::steady_clock::now() + kCommandTimeout;
  while (!result->done) {
    if (spdk_nvme_qpair_process_completions(conn.qpair, 0) < 0) {
      throw std::runtime_error(std::string("NVMe-KV ") + operation +
                               " completion polling failed");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error(std::string("NVMe-KV ") + operation + " timed out");
    }
    std::this_thread::yield();
  }
  if (require_success && !result->success) {
    throw std::runtime_error(std::string("NVMe-KV ") + operation + " failed");
  }
}

void SpdkNvmeKvConnector::ensure_value_size(size_t len) const {
  if (len == 0 || len > max_value_bytes_) {
    throw std::runtime_error("LMCache object length exceeds NVMe-KV value limit");
  }
}

void SpdkNvmeKvConnector::emit_event(const char* phase, const char* operation,
                                     const std::string& key, size_t bytes,
                                     bool success, uint32_t worker_id,
                                     const char* detail,
                                     uint64_t future_id) const {
  if (!trace_events_) {
    return;
  }
  const Key digest = derive_key(key);
  std::fprintf(stderr,
               "LMCACHE_KV_EVENT {\"phase\":\"%s\",\"op\":\"%s\","
               "\"key\":\"%02x%02x%02x%02x\",\"bytes\":%zu,"
               "\"ok\":%s,\"worker\":%u,\"future\":%llu,"
               "\"detail\":\"%s\"}\n",
               phase, operation, digest[0], digest[1], digest[2], digest[3],
               bytes, success ? "true" : "false", worker_id,
               static_cast<unsigned long long>(future_id), detail);
}

}  // namespace lmcache_spdk_kv
