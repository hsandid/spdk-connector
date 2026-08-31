// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "spdk_config.h"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

struct spdk_thread;

namespace lmcache::spdk {

/** Process-wide SPDK application configuration. */
struct SpdkRuntimeConfig {
  std::string json_config_file;
  std::string core_mask;
  int memory_size_mb = 0;
  bool no_huge = false;
  bool no_pci = false;
  std::string print_level;
  std::vector<std::string> log_flags;
  std::string tpoint_group_mask;
  std::size_t io_thread_count = 0;

  bool operator==(const SpdkRuntimeConfig& other) const;
};

/**
 * Owns the one SPDK application allowed in an LMCache process.
 *
 * Connectors acquire this object before opening their selected bdev.  It owns
 * only process-wide state; bdev descriptors, channels, and logical page maps
 * remain owned by their individual ``SpdkBdevContext`` instances.
 */
class SpdkRuntime final {
 public:
  static std::shared_ptr<SpdkRuntime> Acquire(const SpdkConfig& config);

  ~SpdkRuntime();

  SpdkRuntime(const SpdkRuntime&) = delete;
  SpdkRuntime& operator=(const SpdkRuntime&) = delete;

  bool is_available() const noexcept;
  std::string last_error() const;
  struct spdk_thread* app_thread() const noexcept;

  /** Post work to the application thread after checking runtime liveness. */
  void send_message(struct spdk_thread* thread, void (*callback)(void*),
                    void* context) const;
  void ClaimBdev(const std::string& bdev_name);
  void ReleaseBdev(const std::string& bdev_name) noexcept;

 private:
  explicit SpdkRuntime(SpdkRuntimeConfig config);

  static void AppStartCallback(void* context);
  static void StopCallback(void* context);
  void thread_main() noexcept;
  void on_app_start() noexcept;
  void publish_initialization_failure(std::string message) noexcept;
  void stop() noexcept;

  const SpdkRuntimeConfig config_;
  mutable std::mutex state_mutex_;
  std::condition_variable initialization_cv_;
  bool initialization_complete_ = false;
  bool initialization_succeeded_ = false;
  bool stopping_ = false;
  std::string last_error_;
  std::thread runtime_thread_;
  struct spdk_thread* app_thread_ = nullptr;
  std::unordered_set<std::string> claimed_bdevs_;
};

}  // namespace lmcache::spdk
