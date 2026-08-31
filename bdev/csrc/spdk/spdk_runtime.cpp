// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0

#include "spdk_runtime.h"

#include <spdk/event.h>
#include <spdk/log.h>
#include <spdk/string.h>
#include <spdk/thread.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <utility>

namespace lmcache::spdk {
namespace {

std::mutex runtime_registry_mutex;
std::weak_ptr<SpdkRuntime> active_runtime;
bool runtime_started_once = false;

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

std::string ResolveOption(std::string value, const char* environment_name) {
  value = Trim(std::move(value));
  if (!value.empty()) {
    return value;
  }
  const char* environment_value = std::getenv(environment_name);
  return environment_value == nullptr ? std::string() : Trim(environment_value);
}

std::vector<std::string> ParseLogFlags(std::string value) {
  std::vector<std::string> flags;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t delimiter = value.find(',', start);
    std::string flag = Trim(value.substr(start, delimiter - start));
    if (!flag.empty()) {
      flags.push_back(std::move(flag));
    }
    if (delimiter == std::string::npos) {
      break;
    }
    start = delimiter + 1;
  }
  std::sort(flags.begin(), flags.end());
  flags.erase(std::unique(flags.begin(), flags.end()), flags.end());
  return flags;
}

std::optional<enum spdk_log_level> ParsePrintLevel(std::string value) {
  value = Trim(std::move(value));
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  if (value.empty()) {
    return std::nullopt;
  }
  if (value == "disabled") return SPDK_LOG_DISABLED;
  if (value == "error") return SPDK_LOG_ERROR;
  if (value == "warn" || value == "warning") return SPDK_LOG_WARN;
  if (value == "notice") return SPDK_LOG_NOTICE;
  if (value == "info") return SPDK_LOG_INFO;
  if (value == "debug") return SPDK_LOG_DEBUG;
  throw std::invalid_argument(
      "spdk_print_level must be disabled, error, warn, notice, info, or debug");
}

std::string SpdkError(const std::string& operation, int return_code) {
  const int error_number = return_code < 0 ? -return_code : return_code;
  return operation + " failed with code " + std::to_string(return_code) + " (" +
         spdk_strerror(error_number) + ")";
}

SpdkRuntimeConfig MakeRuntimeConfig(const SpdkConfig& config) {
  std::error_code error;
  const std::filesystem::path path =
      std::filesystem::absolute(config.json_config_file, error);
  if (error || !std::filesystem::is_regular_file(path, error)) {
    throw std::invalid_argument("SPDK JSON configuration is not a file: " +
                                config.json_config_file);
  }
  if (config.core_mask.empty() || config.memory_size_mb <= 0 ||
      config.io_thread_count == 0) {
    throw std::invalid_argument("invalid process-wide SPDK runtime configuration");
  }
  SpdkRuntimeConfig result;
  result.json_config_file = path.string();
  result.core_mask = config.core_mask;
  result.memory_size_mb = config.memory_size_mb;
  result.no_huge = config.no_huge;
  result.no_pci = config.no_pci;
  result.print_level = ResolveOption(config.print_level, "LMCACHE_SPDK_PRINT_LEVEL");
  result.log_flags = ParseLogFlags(
      ResolveOption(config.log_flags, "LMCACHE_SPDK_LOG_FLAGS"));
  result.tpoint_group_mask = ResolveOption(
      config.tpoint_group_mask, "LMCACHE_SPDK_TPOINT_GROUP_MASK");
  result.io_thread_count = config.io_thread_count;
  ParsePrintLevel(result.print_level);
  return result;
}

}  // namespace

bool SpdkRuntimeConfig::operator==(const SpdkRuntimeConfig& other) const {
  return json_config_file == other.json_config_file &&
         core_mask == other.core_mask && memory_size_mb == other.memory_size_mb &&
         no_huge == other.no_huge && no_pci == other.no_pci &&
         print_level == other.print_level && log_flags == other.log_flags &&
         tpoint_group_mask == other.tpoint_group_mask &&
         io_thread_count == other.io_thread_count;
}

std::shared_ptr<SpdkRuntime> SpdkRuntime::Acquire(const SpdkConfig& config) {
  const SpdkRuntimeConfig requested = MakeRuntimeConfig(config);
  // SPDK permits one application initialization per process. Share it only
  // when all runtime-wide settings match; bdev contexts retain their own
  // descriptors, channels, placement state, and close lifecycles.
  std::lock_guard<std::mutex> lock(runtime_registry_mutex);
  if (std::shared_ptr<SpdkRuntime> current = active_runtime.lock()) {
    if (!(current->config_ == requested)) {
      throw std::runtime_error(
          "all SPDK adapters in one process must use identical runtime "
          "configuration (JSON path, core mask, memory, PCI/hugepage, logging, "
          "tracepoint, and SPDK I/O-thread settings)");
    }
    return current;
  }
  if (runtime_started_once) {
    throw std::runtime_error(
        "the SPDK application can be initialized only once per process; "
        "start a new process to create another SPDK runtime");
  }
  std::shared_ptr<SpdkRuntime> runtime(new SpdkRuntime(requested));
  active_runtime = runtime;
  return runtime;
}

SpdkRuntime::SpdkRuntime(SpdkRuntimeConfig config) : config_(std::move(config)) {
  // spdk_app_start() blocks its calling thread. Keep that reactor loop on a
  // dedicated C++ thread and wait here until it publishes an app thread or an
  // initialization error to the connector constructing this runtime.
  runtime_thread_ = std::thread(&SpdkRuntime::thread_main, this);
  std::unique_lock<std::mutex> lock(state_mutex_);
  initialization_cv_.wait(lock, [this] { return initialization_complete_; });
  if (initialization_succeeded_) {
    return;
  }
  const std::string error = last_error_;
  lock.unlock();
  if (runtime_thread_.joinable()) {
    runtime_thread_.join();
  }
  throw std::runtime_error(error);
}

SpdkRuntime::~SpdkRuntime() { stop(); }

bool SpdkRuntime::is_available() const noexcept {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return initialization_succeeded_ && !stopping_ && app_thread_ != nullptr;
}

std::string SpdkRuntime::last_error() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return last_error_;
}

struct spdk_thread* SpdkRuntime::app_thread() const noexcept {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return app_thread_;
}

void SpdkRuntime::send_message(struct spdk_thread* thread,
                               void (*callback)(void*), void* context) const {
  if (thread == nullptr || !is_available()) {
    throw std::runtime_error("SPDK runtime is unavailable");
  }
  const int result = spdk_thread_send_msg(thread, callback, context);
  if (result != 0) {
    throw std::runtime_error(SpdkError("posting SPDK message", result));
  }
}

void SpdkRuntime::ClaimBdev(const std::string& bdev_name) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!claimed_bdevs_.insert(bdev_name).second) {
    throw std::runtime_error("SPDK bdev '" + bdev_name +
                             "' is already owned by another L2 adapter");
  }
}

void SpdkRuntime::ReleaseBdev(const std::string& bdev_name) noexcept {
  std::lock_guard<std::mutex> lock(state_mutex_);
  claimed_bdevs_.erase(bdev_name);
}

void SpdkRuntime::AppStartCallback(void* context) {
  static_cast<SpdkRuntime*>(context)->on_app_start();
}

void SpdkRuntime::StopCallback(void*) { spdk_app_stop(0); }

void SpdkRuntime::thread_main() noexcept {
  spdk_app_opts options{};
  spdk_app_opts_init(&options, sizeof(options));
  options.name = "lmcache_spdk";
  options.json_config_file = config_.json_config_file.c_str();
  options.rpc_addr = nullptr;
  options.reactor_mask = config_.core_mask.c_str();
  options.tpoint_group_mask = config_.tpoint_group_mask.empty()
                                 ? nullptr
                                 : config_.tpoint_group_mask.c_str();
  options.mem_size = config_.memory_size_mb;
  options.no_huge = config_.no_huge;
  options.no_pci = config_.no_pci;
  options.disable_signal_handlers = true;
  if (const auto print_level = ParsePrintLevel(config_.print_level)) {
    options.print_level = *print_level;
  }
  for (const std::string& flag : config_.log_flags) {
    if (spdk_log_set_flag(flag.c_str()) != 0) {
      publish_initialization_failure("unknown SPDK log flag '" + flag + "'");
      return;
    }
  }
  // Once spdk_app_start() is entered, SPDK owns this thread until stop() posts
  // spdk_app_stop(). It cannot be initialized again in this process; earlier
  // validation failures are safe to retry with new options.
  runtime_started_once = true;
  const int return_code = spdk_app_start(&options, &SpdkRuntime::AppStartCallback,
                                         this);
  spdk_app_fini();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialization_complete_) {
      last_error_ = SpdkError("SPDK application initialization", return_code);
      initialization_complete_ = true;
      initialization_succeeded_ = false;
    } else if (return_code != 0 && last_error_.empty()) {
      last_error_ = SpdkError("SPDK application shutdown", return_code);
    }
    app_thread_ = nullptr;
  }
  initialization_cv_.notify_all();
}

void SpdkRuntime::on_app_start() noexcept {
  // This callback runs on SPDK's application reactor, making its thread handle
  // the rendezvous point used later for bdev open/close and runtime shutdown.
  std::lock_guard<std::mutex> lock(state_mutex_);
  app_thread_ = spdk_get_thread();
  if (app_thread_ == nullptr) {
    last_error_ = "SPDK started without an application thread";
    initialization_succeeded_ = false;
    initialization_complete_ = true;
    spdk_app_stop(-EIO);
  } else {
    initialization_succeeded_ = true;
    initialization_complete_ = true;
  }
  initialization_cv_.notify_all();
}

void SpdkRuntime::publish_initialization_failure(std::string message) noexcept {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_error_ = std::move(message);
    initialization_succeeded_ = false;
    initialization_complete_ = true;
  }
  initialization_cv_.notify_all();
}

void SpdkRuntime::stop() noexcept {
  struct spdk_thread* app_thread = nullptr;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
    app_thread = app_thread_;
  }
  if (app_thread != nullptr) {
    // Ask SPDK to stop from its owning reactor. The fallback handles the narrow
    // window where message delivery is no longer available during teardown.
    const int result = spdk_thread_send_msg(app_thread, &SpdkRuntime::StopCallback,
                                            nullptr);
    if (result != 0) {
      spdk_app_stop(result);
    }
  }
  if (runtime_thread_.joinable()) {
    runtime_thread_.join();
  }
}

}  // namespace lmcache::spdk
