// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
#include "spdk_bdev_context.h"

#include <spdk/bdev_module.h>

#include <spdk/cpuset.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/log.h>
#include <spdk/string.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace lmcache::spdk {
namespace {

using SteadyClock = std::chrono::steady_clock;

std::uint64_t ElapsedNanoseconds(const SteadyClock::time_point started_at) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          SteadyClock::now() - started_at)
          .count());
}

// Stateless error helpers are file-local. All mutable bdev state is visibly
// owned by SpdkBdevContext below.
std::string SpdkError(const std::string& operation, int return_code) {
  const int error_number = return_code < 0 ? -return_code : return_code;
  return operation + " failed with code " + std::to_string(return_code) + " (" +
         spdk_strerror(error_number) + ")";
}

std::string Trim(std::string value) {
  const auto is_not_space = [](unsigned char character) {
    return !std::isspace(character);
  };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), is_not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), is_not_space).base(),
              value.end());
  return value;
}

bool ParseBoolean(std::string value, const char* description) {
  value = Trim(std::move(value));
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return false;
  }
  throw std::invalid_argument(std::string(description) +
                              " must be a boolean value");
}

bool ResolveStructuredEvents(int configured) {
  if (configured < -1 || configured > 1) {
    throw std::invalid_argument("spdk_structured_events must be -1, 0, or 1");
  }
  if (configured >= 0) {
    return configured == 1;
  }
  const char* environment = std::getenv("LMCACHE_SPDK_STRUCTURED_EVENTS");
  return environment != nullptr &&
         ParseBoolean(environment, "LMCACHE_SPDK_STRUCTURED_EVENTS");
}
}  // namespace

/** Resources that must be created, used, and destroyed on one SPDK thread. */
struct SpdkBdevContext::IoWorker {
  SpdkBdevContext* context = nullptr;
  std::size_t index = 0;
  std::uint32_t core = 0;
  struct spdk_thread* thread = nullptr;
  bool owns_thread = false;
  struct spdk_io_channel* io_channel = nullptr;
  SpdkBufferPool buffer_pool;
  std::uint64_t dma_pool_allocated_bytes = 0;
  std::deque<IoRequest*> pending_io;
  bool draining_pending_io = false;
};

/** Stable per-segment state retained until the SPDK completion callback. */
struct SpdkBdevContext::IoRequest {
  SpdkBdevContext* context = nullptr;
  IoWorker* worker = nullptr;
  IoBatchRequest* batch = nullptr;
  std::size_t result_index = 0;
  Operation operation = Operation::kRead;
  void* buffer = nullptr;
  std::size_t logical_length = 0;
  std::uint64_t byte_offset = 0;
  std::uint64_t physical_length = 0;
  IoTraceContext trace;
  std::uint64_t block_offset = 0;
  std::uint64_t block_count = 0;
  int immediate_submit_result = 0;
  std::optional<std::size_t> buffer_index;
  struct spdk_bdev_io_wait_entry io_wait_entry{};
  SteadyClock::time_point submitted_at;
  std::optional<SteadyClock::time_point> buffer_wait_started_at;
  std::optional<SteadyClock::time_point> retry_wait_started_at;
  std::uint64_t worker_queue_wait_ns = 0;
  std::uint64_t buffer_pool_wait_ns = 0;
  std::uint64_t queue_retry_wait_ns = 0;
  std::uint32_t retry_count = 0;
  bool started = false;
  bool completed = false;
};

/** One message sent to one SPDK I/O worker for its portion of a batch. */
struct SpdkBdevContext::IoWorkerDispatch {
  IoWorker* worker = nullptr;
  std::vector<IoRequest*> requests;
};

/** Shared completion state waited on by one ordinary ConnectorBase worker. */
struct SpdkBdevContext::IoBatchRequest {
  std::mutex completion_mutex;
  std::condition_variable completion_cv;
  std::size_t remaining = 0;
  std::vector<IoResult> results;
  std::vector<std::unique_ptr<IoRequest>> requests;
  std::vector<std::unique_ptr<IoWorkerDispatch>> dispatches;
};

SpdkBdevContext::SpdkBdevContext(SpdkConfig config)
    : config_(std::move(config)),
      structured_events_(ResolveStructuredEvents(config_.structured_events)) {
  if (config_.json_config_file.empty()) {
    throw std::invalid_argument("json_config_file must not be empty");
  }
  if (config_.bdev_name.empty()) {
    throw std::invalid_argument("bdev_name must not be empty");
  }
  if (config_.core_mask.empty()) {
    throw std::invalid_argument("core_mask must not be empty");
  }
  if (config_.memory_size_mb <= 0) {
    throw std::invalid_argument("memory_size_mb must be greater than zero");
  }
  if (config_.io_segment_size == 0) {
    throw std::invalid_argument("io_segment_size must be greater than zero");
  }
  if (config_.spdk_buffer_count == 0) {
    throw std::invalid_argument("spdk_buffer_count must be greater than zero");
  }
  if (config_.io_thread_count == 0) {
    throw std::invalid_argument(
        "spdk_io_thread_count must be greater than zero");
  }

  try {
    // Runtime acquisition may share an existing process-wide SPDK application,
    // but this context exclusively claims one bdev and owns its local channels.
    // Opening must run on the application reactor, not this constructor thread.
    runtime_ = SpdkRuntime::Acquire(config_);
    runtime_->ClaimBdev(config_.bdev_name);
    bdev_claimed_ = true;
    app_thread_ = runtime_->app_thread();
    runtime_->send_message(app_thread_, &SpdkBdevContext::OpenCallback, this);
  } catch (...) {
    if (bdev_claimed_) {
      runtime_->ReleaseBdev(config_.bdev_name);
      bdev_claimed_ = false;
    }
    runtime_.reset();
    throw;
  }

  // Construction stays synchronous for Python callers even though opening the
  // bdev and initializing worker-local resources happen asynchronously.
  std::unique_lock<std::mutex> lock(state_mutex_);
  initialization_cv_.wait(lock, [this] { return initialization_complete_; });
  if (initialization_succeeded_) {
    return;
  }

  const std::string error = last_error_;
  lock.unlock();
  close();
  throw std::runtime_error(error);
}

SpdkBdevContext::~SpdkBdevContext() { close(); }

const StorageGeometry& SpdkBdevContext::geometry() const noexcept {
  return geometry_;
}

bool SpdkBdevContext::is_available() const noexcept {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return initialization_succeeded_ && runtime_ != nullptr &&
         runtime_->is_available() && !closed_ && !shutdown_requested_;
}

std::string SpdkBdevContext::last_error() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return last_error_;
}

IoMetrics SpdkBdevContext::io_metrics() const noexcept {
  return {
      submitted_io_.load(std::memory_order_relaxed),
      completed_io_.load(std::memory_order_relaxed),
      succeeded_io_.load(std::memory_order_relaxed),
      failed_io_.load(std::memory_order_relaxed),
      worker_queue_wait_ns_.load(std::memory_order_relaxed),
      buffer_pool_starvations_.load(std::memory_order_relaxed),
       buffer_pool_wait_ns_.load(std::memory_order_relaxed),
       queue_retries_.load(std::memory_order_relaxed),
       queue_retry_wait_ns_.load(std::memory_order_relaxed),
       dma_buffer_pool_allocated_bytes_.load(std::memory_order_relaxed),
       dma_buffer_pool_in_use_bytes_.load(std::memory_order_relaxed),
       dma_buffer_pool_peak_in_use_bytes_.load(std::memory_order_relaxed),
       submitted_logical_bytes_.load(std::memory_order_relaxed),
       submitted_physical_bytes_.load(std::memory_order_relaxed),
       submitted_unused_dma_buffer_bytes_.load(std::memory_order_relaxed),
       submitted_write_padding_bytes_.load(std::memory_order_relaxed),
  };
}

std::vector<IoResult> SpdkBdevContext::read_batch(
    const std::vector<IoBatchItem>& items) {
  return submit_batch_and_wait(Operation::kRead, items);
}

std::vector<IoResult> SpdkBdevContext::write_batch(
    const std::vector<IoBatchItem>& items) {
  return submit_batch_and_wait(Operation::kWrite, items);
}

void SpdkBdevContext::close() noexcept {
  bool should_request_shutdown = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (closed_) {
      return;
    }
    closed_ = true;
    should_request_shutdown = app_thread_ != nullptr &&
                              (bdev_descriptor_ != nullptr || !io_workers_.empty());
    if (!should_request_shutdown) {
      shutdown_complete_ = true;
    }
  }

  if (should_request_shutdown) {
    request_shutdown(0);
  }
  {
    std::unique_lock<std::mutex> lock(state_mutex_);
    initialization_cv_.wait(lock, [this] { return shutdown_complete_; });
  }
  std::shared_ptr<SpdkRuntime> runtime;
  bool bdev_claimed = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    runtime = std::move(runtime_);
    bdev_claimed = bdev_claimed_;
    bdev_claimed_ = false;
  }
  if (bdev_claimed && runtime != nullptr) {
    runtime->ReleaseBdev(config_.bdev_name);
  }
}

void SpdkBdevContext::OpenCallback(void* context) {
  static_cast<SpdkBdevContext*>(context)->on_runtime_ready();
}

void SpdkBdevContext::BeginShutdownCallback(void* context) {
  static_cast<SpdkBdevContext*>(context)->begin_shutdown_on_app_thread();
}

void SpdkBdevContext::BdevEventCallback(enum spdk_bdev_event_type type,
                                        struct spdk_bdev* bdev, void* context) {
  static_cast<SpdkBdevContext*>(context)->on_bdev_event(type, bdev);
}

void SpdkBdevContext::InitializeIoWorkerCallback(void* context) {
  auto* worker = static_cast<IoWorker*>(context);
  worker->context->initialize_io_worker(worker);
}

void SpdkBdevContext::ExecuteWorkerBatchCallback(void* context) {
  auto* dispatch = static_cast<IoWorkerDispatch*>(context);
  dispatch->worker->context->execute_worker_batch(dispatch);
}

void SpdkBdevContext::ShutdownIoWorkerCallback(void* context) {
  auto* worker = static_cast<IoWorker*>(context);
  worker->context->shutdown_io_worker(worker);
}

void SpdkBdevContext::FinishShutdownCallback(void* context) {
  static_cast<SpdkBdevContext*>(context)->on_io_worker_shutdown();
}

void SpdkBdevContext::RetryIoCallback(void* context) {
  auto* request = static_cast<IoRequest*>(context);
  request->context->execute_io_on_worker(request);
}

void SpdkBdevContext::BdevIoCompletionCallback(struct spdk_bdev_io* bdev_io,
                                               bool success, void* context) {
  auto* request = static_cast<IoRequest*>(context);
  request->context->complete_io_on_worker(request, bdev_io, success);
}

void SpdkBdevContext::on_runtime_ready() noexcept {
  if (app_thread_ == nullptr) {
    publish_initialization_failure(
        "SPDK runtime has no application thread");
    return;
  }

  const int open_result =
      spdk_bdev_open_ext(config_.bdev_name.c_str(), true, &BdevEventCallback,
                         this, &bdev_descriptor_);
  if (open_result != 0) {
    publish_initialization_failure(
        SpdkError("opening bdev '" + config_.bdev_name + "'", open_result));
    request_shutdown(open_result);
    return;
  }

  bdev_ = spdk_bdev_desc_get_bdev(bdev_descriptor_);
  try {
    // Discover bdev constraints before allocating pools. The effective segment
    // size drives both DMA-buffer size and every later bdev command boundary.
    geometry_ = BuildStorageGeometry(
        spdk_bdev_get_num_blocks(bdev_), spdk_bdev_get_block_size(bdev_),
        spdk_bdev_get_write_unit_size(bdev_), spdk_bdev_get_buf_align(bdev_),
        config_.io_segment_size, bdev_->max_rw_size, config_.spdk_buffer_count);

    std::uint32_t core = 0;
    SPDK_ENV_FOREACH_CORE(core) {
      geometry_.spdk_reactor_cores.push_back(core);
    }
    geometry_.spdk_reactor_count = geometry_.spdk_reactor_cores.size();
    if (config_.io_thread_count > geometry_.spdk_reactor_count) {
      throw std::invalid_argument(
          "spdk_io_thread_count=" + std::to_string(config_.io_thread_count) +
          " exceeds the " + std::to_string(geometry_.spdk_reactor_count) +
          " reactor cores selected by core_mask=" + config_.core_mask);
    }
    if (config_.spdk_buffer_count >
        std::numeric_limits<std::size_t>::max() / config_.io_thread_count) {
      throw std::overflow_error("total SPDK buffer count overflows size_t");
    }

    // Put the app reactor first, then use the remaining selected cores in the
    // stable order supplied by SPDK. This makes worker-to-core assignment
    // deterministic without attempting any NUMA policy in the connector.
    const std::uint32_t app_core = spdk_env_get_current_core();
    const auto app_core_position =
        std::find(geometry_.spdk_reactor_cores.begin(),
                  geometry_.spdk_reactor_cores.end(), app_core);
    if (app_core_position == geometry_.spdk_reactor_cores.end()) {
      throw std::runtime_error(
          "SPDK application thread is not running on a selected reactor");
    }
    geometry_.spdk_reactor_cores.erase(app_core_position);
    geometry_.spdk_reactor_cores.insert(geometry_.spdk_reactor_cores.begin(),
                                        app_core);

    geometry_.spdk_io_thread_count = config_.io_thread_count;
    geometry_.spdk_buffer_count = config_.spdk_buffer_count;
    geometry_.total_spdk_buffer_count =
        config_.spdk_buffer_count * config_.io_thread_count;
    geometry_.spdk_io_cores.assign(
        geometry_.spdk_reactor_cores.begin(),
        geometry_.spdk_reactor_cores.begin() + config_.io_thread_count);

    io_workers_.reserve(config_.io_thread_count);
    for (std::size_t index = 0; index < config_.io_thread_count; ++index) {
      auto worker = std::make_unique<IoWorker>();
      worker->context = this;
      worker->index = index;
      worker->core = geometry_.spdk_io_cores[index];
      if (index == 0) {
        // Reuse the already-running application reactor for one channel.
        worker->thread = app_thread_;
      } else {
        struct spdk_cpuset cpumask{};
        spdk_cpuset_zero(&cpumask);
        spdk_cpuset_set_cpu(&cpumask, worker->core, true);
        const std::string name = "lmcache_io_" + std::to_string(index);
        worker->thread = spdk_thread_create(name.c_str(), &cpumask);
        worker->owns_thread = true;
        if (worker->thread == nullptr) {
          throw std::runtime_error("creating SPDK I/O thread " +
                                   std::to_string(index) + " failed");
        }
      }
      io_workers_.push_back(std::move(worker));
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      workers_initializing_ = io_workers_.size();
    }

    // Secondary callbacks may run immediately on their reactors. Worker zero
    // is initialized last and directly because this callback is already on its
    // owning application thread.
    for (std::size_t index = 1; index < io_workers_.size(); ++index) {
      IoWorker* worker = io_workers_[index].get();
      const int send_result = spdk_thread_send_msg(
          worker->thread, &SpdkBdevContext::InitializeIoWorkerCallback, worker);
      if (send_result != 0) {
        on_io_worker_initialized(
            SpdkError("initializing SPDK I/O thread " + std::to_string(index),
                      send_result));
      }
    }
    initialize_io_worker(io_workers_.front().get());
  } catch (const std::exception& error) {
    publish_initialization_failure(error.what());
    request_shutdown(-EINVAL);
  }
}

void SpdkBdevContext::initialize_io_worker(IoWorker* worker) noexcept {
  std::string error;
  try {
    if (spdk_get_thread() != worker->thread) {
      throw std::runtime_error(
          "SPDK I/O worker initialized on the wrong thread");
    }
    spdk_thread_bind(worker->thread, true);
    const std::uint32_t actual_core = spdk_env_get_current_core();
    if (actual_core != worker->core) {
      throw std::runtime_error("SPDK I/O worker " +
                               std::to_string(worker->index) +
                               " was not scheduled on its selected reactor");
    }
    // SPDK channels and DMA allocations are thread-affine. This same worker
    // thread later submits I/O, handles callbacks, and releases both resources.
    worker->io_channel = spdk_bdev_get_io_channel(bdev_descriptor_);
    if (worker->io_channel == nullptr) {
      throw std::runtime_error("creating bdev I/O channel for SPDK worker " +
                               std::to_string(worker->index) + " failed");
    }
    worker->buffer_pool.initialize(geometry_.io_segment_size,
                                   geometry_.buffer_alignment,
                                   geometry_.spdk_buffer_count);
    dma_buffer_pool_allocated_bytes_.fetch_add(
        geometry_.io_segment_size * geometry_.spdk_buffer_count,
        std::memory_order_relaxed);
    worker->dma_pool_allocated_bytes =
        geometry_.io_segment_size * geometry_.spdk_buffer_count;
  } catch (const std::exception& exception) {
    error = exception.what();
  }
  on_io_worker_initialized(std::move(error));
}

void SpdkBdevContext::on_io_worker_initialized(std::string error) noexcept {
  bool initialization_finished = false;
  bool initialization_succeeded = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!error.empty() && last_error_.empty()) {
      last_error_ = std::move(error);
    }
    if (workers_initializing_ != 0) {
      --workers_initializing_;
    }
    initialization_finished = workers_initializing_ == 0;
    if (initialization_finished) {
      initialization_succeeded = last_error_.empty();
      initialization_succeeded_ = initialization_succeeded;
      initialization_complete_ = true;
    }
  }
  if (!initialization_finished) {
    return;
  }
  initialization_cv_.notify_all();
  if (!initialization_succeeded) {
    request_shutdown(-EINVAL);
  }
}

void SpdkBdevContext::on_bdev_event(enum spdk_bdev_event_type type,
                                    struct spdk_bdev*) noexcept {
  if (type == SPDK_BDEV_EVENT_RESIZE) {
    request_shutdown(
        -ENOTSUP, "bdev resize after page-map initialization is unsupported");
  } else if (type == SPDK_BDEV_EVENT_REMOVE) {
    request_shutdown(-ENODEV, "the selected bdev was removed");
  }
}

void SpdkBdevContext::request_shutdown(int return_code,
                                        std::string error) noexcept {
  bool start_shutdown = false;
  std::shared_ptr<SpdkRuntime> runtime;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!error.empty()) {
      last_error_ = std::move(error);
    }
    if (return_code != 0 && shutdown_return_code_ == 0) {
      shutdown_return_code_ = return_code;
    }
    // Resize/remove events, initialization failures, and close() can converge
    // here. Only the first caller posts the app-thread teardown state machine.
    if (!shutdown_requested_ && runtime_ != nullptr &&
        runtime_->is_available()) {
      shutdown_requested_ = true;
      start_shutdown = true;
      runtime = runtime_;
    }
  }
  if (start_shutdown) {
    try {
      runtime->send_message(app_thread_, &SpdkBdevContext::BeginShutdownCallback,
                            this);
    } catch (const std::exception& exception) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (last_error_.empty()) {
        last_error_ = exception.what();
      }
      shutdown_complete_ = true;
      initialization_cv_.notify_all();
    }
  }
}

void SpdkBdevContext::begin_shutdown_on_app_thread() noexcept {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (shutdown_started_) {
      return;
    }
    shutdown_started_ = true;
    workers_shutting_down_ = io_workers_.size();
  }

  if (io_workers_.empty()) {
    finish_shutdown_on_app_thread();
    return;
  }

  // Dispatch secondary releases before releasing the application worker. Each
  // secondary acknowledges back to the app thread, which owns final shutdown.
  for (std::size_t index = 1; index < io_workers_.size(); ++index) {
    IoWorker* worker = io_workers_[index].get();
    const int send_result = spdk_thread_send_msg(
        worker->thread, &SpdkBdevContext::ShutdownIoWorkerCallback, worker);
    if (send_result != 0) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (last_error_.empty()) {
          last_error_ = SpdkError(
              "stopping SPDK I/O thread " + std::to_string(index), send_result);
        }
      }
      // This is an abnormal runtime teardown path. Release DMA allocations to
      // keep the C++ lifetime safe; SPDK finalization owns any channel cleanup.
      worker->buffer_pool.release_all();
      if (worker->dma_pool_allocated_bytes != 0) {
        dma_buffer_pool_allocated_bytes_.fetch_sub(
            worker->dma_pool_allocated_bytes,
            std::memory_order_relaxed);
        worker->dma_pool_allocated_bytes = 0;
      }
      worker->io_channel = nullptr;
      on_io_worker_shutdown();
    }
  }
  shutdown_io_worker(io_workers_.front().get());
}

void SpdkBdevContext::shutdown_io_worker(IoWorker* worker) noexcept {
  // These requests never reached SPDK because pool backpressure held them in
  // this worker's FIFO. Complete them before releasing their batch-owned state.
  while (!worker->pending_io.empty()) {
    IoRequest* request = worker->pending_io.front();
    worker->pending_io.pop_front();
    complete_request(request, false, "SPDK runtime is shutting down");
  }
  // Both the channel and its DMA pool must be destroyed on their owner thread.
  worker->buffer_pool.release_all();
  if (worker->dma_pool_allocated_bytes != 0) {
    dma_buffer_pool_allocated_bytes_.fetch_sub(
        worker->dma_pool_allocated_bytes,
        std::memory_order_relaxed);
    worker->dma_pool_allocated_bytes = 0;
  }
  if (worker->io_channel != nullptr) {
    spdk_put_io_channel(worker->io_channel);
    worker->io_channel = nullptr;
  }

  if (!worker->owns_thread) {
    on_io_worker_shutdown();
    return;
  }

  const int send_result = spdk_thread_send_msg(
      app_thread_, &SpdkBdevContext::FinishShutdownCallback, this);
  spdk_thread_exit(worker->thread);
  if (send_result != 0) {
    // The application thread is still polling this direct shutdown callback,
    // so fall back to the process-global app shutdown instead of hanging.
    spdk_app_stop(send_result);
  }
}

void SpdkBdevContext::on_io_worker_shutdown() noexcept {
  bool all_workers_stopped = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (workers_shutting_down_ != 0) {
      --workers_shutting_down_;
    }
    all_workers_stopped = workers_shutting_down_ == 0;
  }
  if (all_workers_stopped) {
    finish_shutdown_on_app_thread();
  }
}

void SpdkBdevContext::finish_shutdown_on_app_thread() noexcept {
  release_descriptor_on_app_thread();
  dma_buffer_pool_in_use_bytes_.store(0, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    shutdown_complete_ = true;
  }
  initialization_cv_.notify_all();
}

void SpdkBdevContext::release_descriptor_on_app_thread() noexcept {
  if (bdev_descriptor_ != nullptr) {
    spdk_bdev_close(bdev_descriptor_);
    bdev_descriptor_ = nullptr;
  }
  bdev_ = nullptr;
}

void SpdkBdevContext::publish_initialization_failure(
    std::string message) noexcept {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_error_ = std::move(message);
    initialization_succeeded_ = false;
    initialization_complete_ = true;
  }
  initialization_cv_.notify_all();
}

std::vector<IoResult> SpdkBdevContext::submit_batch_and_wait(
    Operation operation, const std::vector<IoBatchItem>& items) {
  if (items.empty()) {
    return {};
  }

  std::size_t worker_count = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (closed_ || shutdown_requested_ || !initialization_succeeded_) {
      const std::string detail = last_error_.empty() ? "" : ": " + last_error_;
      throw std::runtime_error("SPDK runtime is not available" + detail);
    }
    worker_count = io_workers_.size();
  }
  if (worker_count == 0) {
    throw std::runtime_error("SPDK runtime has no I/O workers");
  }

  IoBatchRequest batch;
  batch.remaining = items.size();
  batch.results.resize(items.size());
  batch.requests.reserve(items.size());
  batch.dispatches.resize(worker_count);

  const std::size_t first_worker =
      next_io_worker_.fetch_add(1, std::memory_order_relaxed) % worker_count;
  for (std::size_t index = 0; index < items.size(); ++index) {
    const IoBatchItem& item = items[index];
    validate_item(item);
    const std::size_t worker_index = (first_worker + index) % worker_count;
    if (!batch.dispatches[worker_index]) {
      batch.dispatches[worker_index] = std::make_unique<IoWorkerDispatch>();
      batch.dispatches[worker_index]->worker = io_workers_[worker_index].get();
    }

    auto request = std::make_unique<IoRequest>();
    request->context = this;
    request->worker = io_workers_[worker_index].get();
    request->batch = &batch;
    request->result_index = index;
    request->operation = operation;
    request->buffer = item.buffer;
    request->logical_length = item.logical_length;
    request->byte_offset = item.byte_offset;
    request->physical_length = item.physical_length;
    request->trace = item.trace;
    request->submitted_at = SteadyClock::now();
    submitted_io_.fetch_add(1, std::memory_order_relaxed);
    batch.dispatches[worker_index]->requests.push_back(request.get());
    batch.requests.push_back(std::move(request));
  }

  // Request and dispatch objects are heap-stable inside this stack-owned batch.
  // Waiting for remaining==0 guarantees no callback retains either pointer.
  for (const std::unique_ptr<IoWorkerDispatch>& dispatch : batch.dispatches) {
    if (!dispatch) {
      continue;
    }
    try {
      send_message(dispatch->worker->thread,
                   &SpdkBdevContext::ExecuteWorkerBatchCallback,
                   dispatch.get());
    } catch (const std::exception& error) {
      for (IoRequest* request : dispatch->requests) {
        complete_request(request, false, error.what());
      }
    }
  }

  std::unique_lock<std::mutex> lock(batch.completion_mutex);
  batch.completion_cv.wait(lock, [&batch] { return batch.remaining == 0; });
  return std::move(batch.results);
}

void SpdkBdevContext::send_message(struct spdk_thread* thread,
                                   void (*callback)(void*), void* context) {
  if (thread == nullptr || callback == nullptr) {
    throw std::invalid_argument("SPDK thread and callback must not be null");
  }
  std::shared_ptr<SpdkRuntime> runtime;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (closed_ || shutdown_requested_ || runtime_ == nullptr) {
      const std::string detail = last_error_.empty() ? "" : ": " + last_error_;
      throw std::runtime_error("SPDK runtime is not available" + detail);
    }
    runtime = runtime_;
  }
  runtime->send_message(thread, callback, context);
}

void SpdkBdevContext::execute_worker_batch(
    IoWorkerDispatch* dispatch) noexcept {
  for (IoRequest* request : dispatch->requests) {
    execute_io_on_worker(request);
  }
}

void SpdkBdevContext::execute_io_on_worker(IoRequest* request) noexcept {
  IoWorker* worker = request->worker;
  try {
    if (spdk_get_thread() != worker->thread) {
      throw std::runtime_error("SPDK I/O executed on the wrong worker thread");
    }
    if (!request->started) {
      request->started = true;
      request->worker_queue_wait_ns = ElapsedNanoseconds(request->submitted_at);
      worker_queue_wait_ns_.fetch_add(request->worker_queue_wait_ns,
                                      std::memory_order_relaxed);
    }
    if (request->retry_wait_started_at.has_value()) {
      const std::uint64_t waited =
          ElapsedNanoseconds(*request->retry_wait_started_at);
      request->queue_retry_wait_ns += waited;
      queue_retry_wait_ns_.fetch_add(waited, std::memory_order_relaxed);
      request->retry_wait_started_at.reset();
    }
    // A single worker exclusively owns this pool, so acquisition needs no
    // mutex. Exhaustion is connector-side backpressure, not an I/O failure.
    const std::optional<std::size_t> buffer = worker->buffer_pool.try_acquire();
    if (!buffer.has_value()) {
      if (!request->buffer_wait_started_at.has_value()) {
        request->buffer_wait_started_at = SteadyClock::now();
        buffer_pool_starvations_.fetch_add(1, std::memory_order_relaxed);
      }
      // Retain the stable request in FIFO order until a callback returns a
      // DMA lease, then drain_pending_io() resumes it on this same worker.
      worker->pending_io.push_back(request);
      return;
    }
    request->buffer_index = *buffer;
    const std::uint64_t in_use = dma_buffer_pool_in_use_bytes_.fetch_add(
        geometry_.io_segment_size, std::memory_order_relaxed) +
        geometry_.io_segment_size;
    std::uint64_t peak = dma_buffer_pool_peak_in_use_bytes_.load(
        std::memory_order_relaxed);
    while (peak < in_use &&
           !dma_buffer_pool_peak_in_use_bytes_.compare_exchange_weak(
               peak, in_use, std::memory_order_relaxed)) {
    }
    if (request->buffer_wait_started_at.has_value()) {
      const std::uint64_t waited =
          ElapsedNanoseconds(*request->buffer_wait_started_at);
      request->buffer_pool_wait_ns += waited;
      buffer_pool_wait_ns_.fetch_add(waited, std::memory_order_relaxed);
      request->buffer_wait_started_at.reset();
    }
    void* io_buffer = worker->buffer_pool.data(*request->buffer_index);
    if (request->operation == Operation::kWrite) {
      std::memset(io_buffer, 0,
                  static_cast<std::size_t>(request->physical_length));
      std::memcpy(io_buffer, request->buffer, request->logical_length);
    }

    // SPDK's block API consumes block counts. Submit physical length so the
    // final page tail is written/read consistently with extent allocation.
    request->block_offset = request->byte_offset / geometry_.block_size;
    request->block_count = request->physical_length / geometry_.block_size;
    if (request->operation == Operation::kRead) {
      request->immediate_submit_result = spdk_bdev_read_blocks(
          bdev_descriptor_, worker->io_channel, io_buffer,
          request->block_offset, request->block_count,
          &SpdkBdevContext::BdevIoCompletionCallback, request);
    } else {
      request->immediate_submit_result = spdk_bdev_write_blocks(
          bdev_descriptor_, worker->io_channel, io_buffer,
          request->block_offset, request->block_count,
          &SpdkBdevContext::BdevIoCompletionCallback, request);
    }
    emit_io_event(*request, "submit", nullptr);

    if (request->immediate_submit_result == -ENOMEM) {
      // Queue exhaustion is temporary. Relinquish this worker's scarce SPDK
      // buffer while its channel waits for capacity, then retry on this thread.
      release_buffer(request);
      ++request->retry_count;
      queue_retries_.fetch_add(1, std::memory_order_relaxed);
      request->retry_wait_started_at = SteadyClock::now();
      request->io_wait_entry = {};
      request->io_wait_entry.bdev = bdev_;
      request->io_wait_entry.cb_fn = &SpdkBdevContext::RetryIoCallback;
      request->io_wait_entry.cb_arg = request;
      const int wait_result = spdk_bdev_queue_io_wait(bdev_, worker->io_channel,
                                                      &request->io_wait_entry);
      if (wait_result == 0) {
        return;
      }
      if (wait_result == -EINVAL &&
          spdk_thread_send_msg(worker->thread,
                               &SpdkBdevContext::RetryIoCallback,
                               request) == 0) {
        return;
      }
      complete_request(request, false,
                       SpdkError("queueing SPDK bdev I/O wait", wait_result));
      drain_pending_io(worker);
      return;
    }
    if (request->immediate_submit_result != 0) {
      release_buffer(request);
      complete_request(request, false,
                       SpdkError("submitting SPDK bdev I/O",
                                 request->immediate_submit_result));
      drain_pending_io(worker);
      return;
    }
    submitted_logical_bytes_.fetch_add(request->logical_length,
                                       std::memory_order_relaxed);
    submitted_physical_bytes_.fetch_add(request->physical_length,
                                        std::memory_order_relaxed);
    submitted_unused_dma_buffer_bytes_.fetch_add(
        geometry_.io_segment_size - request->physical_length,
        std::memory_order_relaxed);
    if (request->operation == Operation::kWrite) {
      submitted_write_padding_bytes_.fetch_add(
          request->physical_length - request->logical_length,
          std::memory_order_relaxed);
    }
  } catch (const std::exception& error) {
    release_buffer(request);
    complete_request(request, false, error.what());
    drain_pending_io(worker);
  }
}

void SpdkBdevContext::complete_io_on_worker(IoRequest* request,
                                            struct spdk_bdev_io* bdev_io,
                                            bool success) noexcept {
  IoWorker* worker = request->worker;
  // SPDK transfers ownership of this completion object to the callback.
  spdk_bdev_free_io(bdev_io);
  emit_io_event(*request, "callback", &success);
  try {
    if (success && request->operation == Operation::kRead) {
      // Reads land in DMA memory first; copy only logical caller bytes before
      // returning the lease, whose contents may be reused immediately.
      std::memcpy(request->buffer,
                  worker->buffer_pool.data(*request->buffer_index),
                  request->logical_length);
    }
    release_buffer(request);
    complete_request(request, success,
                     success ? std::string() : "SPDK bdev I/O failed");
  } catch (const std::exception& error) {
    release_buffer(request);
    complete_request(request, false, error.what());
  }
  drain_pending_io(worker);
}

void SpdkBdevContext::release_buffer(IoRequest* request) noexcept {
  if (!request->buffer_index.has_value()) {
    return;
  }
  try {
    request->worker->buffer_pool.release(*request->buffer_index);
    dma_buffer_pool_in_use_bytes_.fetch_sub(geometry_.io_segment_size,
                                            std::memory_order_relaxed);
  } catch (...) {
  }
  request->buffer_index.reset();
}

void SpdkBdevContext::drain_pending_io(IoWorker* worker) noexcept {
  // A completion can release a buffer while this loop submits another request.
  // Prevent recursive drains from reordering or double-advancing the FIFO.
  if (worker->draining_pending_io) {
    return;
  }
  worker->draining_pending_io = true;
  while (!worker->pending_io.empty() && worker->buffer_pool.available() != 0) {
    IoRequest* request = worker->pending_io.front();
    worker->pending_io.pop_front();
    execute_io_on_worker(request);
  }
  worker->draining_pending_io = false;
}

void SpdkBdevContext::complete_request(IoRequest* request, bool success,
                                        std::string error) noexcept {
  if (request->completed) {
    return;
  }
  request->completed = true;
  completed_io_.fetch_add(1, std::memory_order_relaxed);
  (success ? succeeded_io_ : failed_io_).fetch_add(1,
                                                    std::memory_order_relaxed);
  IoBatchRequest* batch = request->batch;
  bool batch_complete = false;
  {
    std::lock_guard<std::mutex> lock(batch->completion_mutex);
    batch->results[request->result_index] = IoResult{success, std::move(error)};
    batch_complete = --batch->remaining == 0;
  }
  if (batch_complete) {
    batch->completion_cv.notify_one();
  }
}

void SpdkBdevContext::emit_io_event(
    const IoRequest& request, const char* phase,
    const bool* callback_success) const noexcept {
  if (!structured_events_) {
    return;
  }
  try {
    std::ostringstream event;
    event << "{\"schema\":\"lmcache.spdk.io.v3\",\"phase\":\"" << phase
          << "\",\"operation\":\""
          << (request.operation == Operation::kRead ? "get" : "set")
          << "\",\"future_id\":" << request.trace.future_id
          << ",\"item_index\":" << request.trace.item_index
          << ",\"segment_index\":" << request.trace.segment_index
          << ",\"segment_count\":" << request.trace.segment_count
          << ",\"first_page\":" << request.trace.first_page
          << ",\"page_count\":" << request.trace.page_count
          << ",\"logical_offset\":" << request.trace.logical_offset
          << ",\"logical_bytes\":" << request.logical_length
          << ",\"physical_bytes\":" << request.physical_length
          << ",\"io_thread_index\":" << request.worker->index
          << ",\"spdk_core\":" << request.worker->core << ",\"buffer_index\":";
    if (request.buffer_index.has_value()) {
      event << *request.buffer_index;
    } else {
      event << "null";
    }
    event << ",\"block_offset\":" << request.block_offset
           << ",\"block_count\":" << request.block_count
           << ",\"immediate_submit_result\":" << request.immediate_submit_result
           << ",\"worker_queue_wait_ns\":" << request.worker_queue_wait_ns
           << ",\"buffer_pool_wait_ns\":" << request.buffer_pool_wait_ns
           << ",\"queue_retry_wait_ns\":" << request.queue_retry_wait_ns
           << ",\"queue_retry_count\":" << request.retry_count
           << ",\"buffer_pool_available\":"
           << request.worker->buffer_pool.available()
           << ",\"callback_success\":";
    if (callback_success == nullptr) {
      event << "null";
    } else {
      event << (*callback_success ? "true" : "false");
    }
    event << '}';
    std::fprintf(stderr, "LMCACHE_SPDK_EVENT %s\n", event.str().c_str());
    std::fflush(stderr);
  } catch (...) {
  }
}

void SpdkBdevContext::validate_item(const IoBatchItem& item) const {
  if (item.buffer == nullptr) {
    throw std::invalid_argument("buffer must not be null");
  }
  if (item.logical_length == 0) {
    throw std::invalid_argument("logical segment length must be nonzero");
  }
  if (item.physical_length < item.logical_length ||
      item.physical_length > geometry_.io_segment_size) {
    throw std::invalid_argument(
        "physical segment length must cover logical bytes and not exceed "
        "io_segment_size");
  }
  if (item.byte_offset % geometry_.page_size != 0 ||
      item.physical_length % geometry_.page_size != 0) {
    throw std::invalid_argument(
        "segment offset and physical length must be storage-page aligned");
  }
  if (item.byte_offset > geometry_.usable_capacity_bytes ||
      item.physical_length >
          geometry_.usable_capacity_bytes - item.byte_offset) {
    throw std::invalid_argument("segment lies outside usable bdev capacity");
  }
}

}  // namespace lmcache::spdk
