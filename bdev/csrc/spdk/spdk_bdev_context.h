// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "storage_geometry.h"
#include "spdk_buffer_pool.h"
#include "spdk_config.h"
#include "spdk_runtime.h"

#include <spdk/bdev.h>
#include <spdk/thread.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lmcache::spdk {

/** Correlation and geometry fields attached to one physical I/O segment. */
struct IoTraceContext {
  std::uint64_t future_id = 0;
  std::size_t item_index = 0;
  std::size_t segment_index = 0;
  std::size_t segment_count = 0;
  std::uint64_t first_page = 0;
  std::uint64_t page_count = 0;
  std::uint64_t logical_offset = 0;
};

/** One physical segment of a caller-visible object. */
struct IoBatchItem {
  /** Start of this segment in caller-owned storage. */
  void* buffer = nullptr;

  /** Caller bytes copied to or from the SPDK buffer. */
  std::size_t logical_length = 0;

  /** Physical byte offset on the selected bdev. */
  std::uint64_t byte_offset = 0;

  /** Page-aligned bdev byte count, including final zero padding. */
  std::uint64_t physical_length = 0;

  IoTraceContext trace;
};

/** Per-segment result returned after its SPDK callback completes. */
struct IoResult {
  bool success = false;
  std::string error;
};

/** Process-lifetime counters for physical bdev segments in this context. */
struct IoMetrics {
  std::uint64_t submitted = 0;
  std::uint64_t completed = 0;
  std::uint64_t succeeded = 0;
  std::uint64_t failed = 0;
  std::uint64_t worker_queue_wait_ns = 0;
  std::uint64_t buffer_pool_starvations = 0;
  std::uint64_t buffer_pool_wait_ns = 0;
  std::uint64_t queue_retries = 0;
  std::uint64_t queue_retry_wait_ns = 0;
  std::uint64_t dma_buffer_pool_allocated_bytes = 0;
  std::uint64_t dma_buffer_pool_in_use_bytes = 0;
  std::uint64_t dma_buffer_pool_peak_in_use_bytes = 0;
  std::uint64_t submitted_logical_bytes = 0;
  std::uint64_t submitted_physical_bytes = 0;
  std::uint64_t submitted_unused_dma_buffer_bytes = 0;
  std::uint64_t submitted_write_padding_bytes = 0;
};

/**
 * Owns one bdev's SPDK resources inside the process-wide runtime.
 *
 * ConnectorBase worker threads call read_batch()/write_batch() with plain
 * caller buffers. This class distributes segments across logical SPDK I/O
 * workers. Each owns one thread, bdev channel, buffer pool, and pending queue.
 * Callbacks release that worker's buffers and wake the waiting ConnectorBase
 * worker only after the whole segment batch is complete.
 *
 * The mutex and condition variable below bridge ordinary C++ worker threads
 * to SPDK's message/callback model; they never protect or drive SPDK polling.
 * Per-worker resources are used only on their owning SPDK thread; descriptor
 * lifecycle remains on the shared runtime's application thread.
 */
class SpdkBdevContext final {
 public:
  /** Open one bdev and construct its channel/buffer owners. */
  explicit SpdkBdevContext(SpdkConfig config);

  /** Release this bdev's resources on their owning SPDK threads. */
  ~SpdkBdevContext();

  SpdkBdevContext(const SpdkBdevContext&) = delete;
  SpdkBdevContext& operator=(const SpdkBdevContext&) = delete;

  /** Return immutable geometry discovered from the opened bdev. */
  const StorageGeometry& geometry() const noexcept;

  /** Return whether the bdev runtime can accept new I/O. */
  bool is_available() const noexcept;

  /** Return the most recent native runtime error, if any. */
  std::string last_error() const;

  /** Return aggregate timing and backpressure metrics for physical I/O. */
  IoMetrics io_metrics() const noexcept;

  /** Submit every read segment before waiting for their callbacks. */
  std::vector<IoResult> read_batch(const std::vector<IoBatchItem>& items);

  /** Submit every write segment before waiting for their callbacks. */
  std::vector<IoResult> write_batch(const std::vector<IoBatchItem>& items);

  /** Release this bdev's resources. Safe to repeat. */
  void close() noexcept;

 private:
  enum class Operation { kRead, kWrite };
  struct IoRequest;
  struct IoWorker;
  struct IoWorkerDispatch;
  struct IoBatchRequest;

  // Static adapters required by SPDK's C callback API. The void pointers are
  // non-owning and remain valid until the waiting worker observes completion.
  static void OpenCallback(void* context);
  static void BeginShutdownCallback(void* context);
  static void BdevEventCallback(enum spdk_bdev_event_type type,
                                struct spdk_bdev* bdev, void* context);
  static void InitializeIoWorkerCallback(void* context);
  static void ExecuteWorkerBatchCallback(void* context);
  static void ShutdownIoWorkerCallback(void* context);
  static void FinishShutdownCallback(void* context);
  static void RetryIoCallback(void* context);
  static void BdevIoCompletionCallback(struct spdk_bdev_io* bdev_io,
                                       bool success, void* context);

  /** Open the bdev and schedule creation of every thread-owned channel. */
  void on_runtime_ready() noexcept;

  /** Obtain one channel and buffer pool on their owning SPDK thread. */
  void initialize_io_worker(IoWorker* worker) noexcept;

  /** Publish success or begin all-worker unwind after the final initializer. */
  void on_io_worker_initialized(std::string error) noexcept;

  /** Convert remove/resize notifications into connector shutdown. */
  void on_bdev_event(enum spdk_bdev_event_type type,
                     struct spdk_bdev* bdev) noexcept;

  /** Mark this bdev unavailable and start its local resource teardown. */
  void request_shutdown(int return_code, std::string error = {}) noexcept;

  /** Dispatch resource release to every owning SPDK thread. */
  void begin_shutdown_on_app_thread() noexcept;

  /** Release one channel and buffer pool on their owning SPDK thread. */
  void shutdown_io_worker(IoWorker* worker) noexcept;

  /** Finish shutdown after every I/O worker acknowledges resource release. */
  void on_io_worker_shutdown() noexcept;

  /** Close the descriptor and complete this bdev's teardown; app thread only. */
  void finish_shutdown_on_app_thread() noexcept;

  /** Close resources shared by all I/O workers; app thread only. */
  void release_descriptor_on_app_thread() noexcept;

  /** Publish a constructor-visible error and wake the constructing thread. */
  void publish_initialization_failure(std::string message) noexcept;

  /**
   * Build stable request state, post it to SPDK workers, and wait for callbacks.
   *
   * The ordinary caller thread owns the stack batch and waits until every SPDK
   * callback has released its retained request pointer. SPDK workers alone
   * acquire DMA buffers, submit bdev commands, and complete the batch.
   */
  std::vector<IoResult> submit_batch_and_wait(
      Operation operation, const std::vector<IoBatchItem>& items);

  /** Post one callback to a selected SPDK thread after a lifecycle check. */
  void send_message(struct spdk_thread* thread, void (*callback)(void*),
                    void* context);

  /** Start all segments assigned to one thread by the batch dispatcher. */
  void execute_worker_batch(IoWorkerDispatch* dispatch) noexcept;

  /** Submit one operation on its channel, staging only when configured. */
  void execute_io_on_worker(IoRequest* request) noexcept;

  /** Finish one operation on the same SPDK thread that submitted it. */
  void complete_io_on_worker(IoRequest* request, struct spdk_bdev_io* bdev_io,
                             bool success) noexcept;

  /** Return an optional buffer lease to its owning worker's pool. */
  void release_buffer(IoRequest* request) noexcept;

  /** Submit pending staged segments as that worker's callbacks free buffers. */
  void drain_pending_io(IoWorker* worker) noexcept;

  /** Store one final result and notify the worker after the last segment. */
  void complete_request(IoRequest* request, bool success,
                        std::string error) noexcept;

  /** Emit one optional versioned JSON diagnostic record. */
  void emit_io_event(const IoRequest& request, const char* phase,
                     const bool* callback_success) const noexcept;

  /** Enforce page alignment, segment bounds, and caller-buffer validity. */
  void validate_item(const IoBatchItem& item) const;

  SpdkConfig config_;
  std::shared_ptr<SpdkRuntime> runtime_;
  const bool structured_events_;

  // Protects initialization publication, asynchronous device events, and close.
  mutable std::mutex state_mutex_;
  std::condition_variable initialization_cv_;
  bool initialization_complete_ = false;
  bool initialization_succeeded_ = false;
  bool closed_ = false;
  std::string last_error_;
  bool shutdown_requested_ = false;
  bool shutdown_started_ = false;
  bool shutdown_complete_ = false;
  bool bdev_claimed_ = false;
  std::size_t workers_initializing_ = 0;
  std::size_t workers_shutting_down_ = 0;
  int shutdown_return_code_ = 0;

  // Published before construction returns and read-only afterward.
  StorageGeometry geometry_;
  // The application thread belongs to ``runtime_``. The descriptor is shared
  // by channels but opened and closed only on that thread.
  struct spdk_thread* app_thread_ = nullptr;
  struct spdk_bdev* bdev_ = nullptr;
  struct spdk_bdev_desc* bdev_descriptor_ = nullptr;

  // Each worker is pinned to a distinct selected reactor and owns exactly one
  // bdev I/O channel, buffer pool, and pending queue. The wrappers remain
  // stable until this context's close handshake completes.
  std::vector<std::unique_ptr<IoWorker>> io_workers_;
  std::atomic<std::size_t> next_io_worker_{0};
  std::atomic<std::uint64_t> submitted_io_{0};
  std::atomic<std::uint64_t> completed_io_{0};
  std::atomic<std::uint64_t> succeeded_io_{0};
  std::atomic<std::uint64_t> failed_io_{0};
  std::atomic<std::uint64_t> worker_queue_wait_ns_{0};
  std::atomic<std::uint64_t> buffer_pool_starvations_{0};
  std::atomic<std::uint64_t> buffer_pool_wait_ns_{0};
  std::atomic<std::uint64_t> queue_retries_{0};
  std::atomic<std::uint64_t> queue_retry_wait_ns_{0};
  std::atomic<std::uint64_t> dma_buffer_pool_allocated_bytes_{0};
  std::atomic<std::uint64_t> dma_buffer_pool_in_use_bytes_{0};
  std::atomic<std::uint64_t> dma_buffer_pool_peak_in_use_bytes_{0};
  std::atomic<std::uint64_t> submitted_logical_bytes_{0};
  std::atomic<std::uint64_t> submitted_physical_bytes_{0};
  std::atomic<std::uint64_t> submitted_unused_dma_buffer_bytes_{0};
  std::atomic<std::uint64_t> submitted_write_padding_bytes_{0};
};

}  // namespace lmcache::spdk
