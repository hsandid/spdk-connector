// SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
// Native-plugin pybind connector backed by ephemeral page extents.

#include "connector_base.h"
#include "connector_pybind_utils.h"
#include "page_extent_allocator.h"
#include "spdk_bdev_context.h"
#include "spdk_pybind_common.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace lmcache::spdk::pybind {
namespace {

struct SpdkConnectorConnection {};

/**
 * Native-plugin connector with ephemeral, per-key placement state.
 *
 * ConnectorBase owns request/completion queues and worker lifetimes; this class
 * owns the page allocator and serializes each key with its own mutex. Same-size
 * replacement is copy-on-write: new extents publish only after all physical
 * writes succeed, while size changes retain LMCache's immutable-key behavior.
 * SPDK runtime and bdev channel ownership remain in SpdkBdevContext.
 */
class SpdkConnector final
    : public lmcache::connector::ConnectorBase<SpdkConnectorConnection> {
public:
  SpdkConnector(const std::string &json, const std::string &bdev,
                const std::vector<int> &reactors,
                const std::vector<int> &workers, const std::size_t threads,
                const std::size_t buffers, const int memory, const bool no_huge,
                const bool no_pci, const std::uint64_t segment)
      : ConnectorBase(static_cast<int>(workers.size()),
                      lmcache::connector::WorkerPoolConfig{}),
        context_(MakeConfig(json, bdev, reactors, threads, buffers, memory,
                            no_huge, no_pci, segment)),
        allocator_(context_.geometry().page_count), worker_cpus_(workers) {
    if (worker_cpus_.empty())
      throw std::invalid_argument("connector_worker_cpus must not be empty");
    start_workers();
  }
  ~SpdkConnector() override { close(); }
  void close() override {
    ConnectorBase::close();
    context_.close();
  }
  py::dict runtime_status() const {
    py::dict result;
    result["is_available"] = context_.is_available();
    result["last_error"] = context_.last_error();
    result["effective_io_segment_bytes"] = context_.geometry().io_segment_size;
    result["bdev_max_rw_size_bytes"] =
        context_.geometry().bdev_max_rw_size_bytes;
    result["io_metrics"] = IoMetricsDict(context_.io_metrics());
    return result;
  }

protected:
  SpdkConnectorConnection create_connection() override {
    // ConnectorBase invokes this in each ordinary request worker, not on an
    // SPDK reactor. Pinning keeps metadata/batch preparation off reactor CPUs.
    const int cpu = worker_cpus_.at(
        next_worker_cpu_.fetch_add(1, std::memory_order_relaxed) %
        worker_cpus_.size());
    if (cpu >= CPU_SETSIZE)
      throw std::invalid_argument("connector worker CPU exceeds CPU_SETSIZE");
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
      throw std::runtime_error("cannot pin connector worker to CPU " +
                               std::to_string(cpu) + ": errno " +
                               std::to_string(errno));
    }
    return {};
  }
  std::size_t choose_num_tiles(lmcache::connector::Op op,
                                std::size_t count) const override {
    if (op == lmcache::connector::Op::BATCH_TILE_SET)
      return 1;  // Publish one all-or-nothing extent reservation per Store future.
    return lmcache::connector::ConnectorBase<
        SpdkConnectorConnection>::choose_num_tiles(op, count);
  }
  void do_single_get(SpdkConnectorConnection &, const std::string &key,
                      void *buffer, const size_t length, size_t) override {
    // Keep the entry stable until every segment has copied into the caller.
    const auto state = StateFor(key);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->entry || length != state->entry->logical_size)
      throw std::runtime_error(
          "key is missing or destination size does not match");
    Read(*state->entry, buffer);
  }
  void do_single_set(SpdkConnectorConnection &, const std::string &key,
                     const void *buffer, const size_t length, size_t) override {
    if (length == 0)
      throw std::invalid_argument("values must not be empty");
    const auto state = StateFor(key);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->entry && state->entry->logical_size != length)
      return;  // Preserve the established immutable behavior across size changes.
    // Do not publish replacement placement until its complete bdev write ends.
    Entry replacement = AllocateEntry(length);
    try {
      Write(replacement, buffer);
    } catch (...) {
      allocator_.release(replacement.extents);
      throw;
    }
    std::optional<Entry> previous = std::move(state->entry);
    state->entry = std::move(replacement);
    if (previous)
      allocator_.release(previous->extents);
  }
  void do_batch_get(SpdkConnectorConnection &,
                     const lmcache::connector::Request &request) override {
    const auto states = StatesFor(request.keys);
    const auto locks = LockStates(states);
    // Hold every affected key lock through synchronous bdev I/O. A concurrent
    // Store/Delete therefore cannot free or repurpose extents being read.
    std::vector<IoBatchItem> items;
    std::vector<std::size_t> item_to_key;
    std::vector<bool> success(request.keys.size(), true);
    for (std::size_t i = 0; i < request.keys.size(); ++i) {
      const auto &entry = states[i]->entry;
      if (!entry || request.buf_lens[i] != entry->logical_size) {
        success[i] = false;
        continue;
      }
      AppendSegments(*entry, request.buf_ptrs[i], i, &items, &item_to_key);
    }
    try {
      const auto results =
          items.empty() ? std::vector<IoResult>() : context_.read_batch(items);
      if (results.size() != items.size())
        throw std::runtime_error(
            "bdev read batch returned an unexpected result count");
      for (std::size_t i = 0; i < results.size(); ++i)
        if (!results[i].success)
          success[item_to_key[i]] = false;
    } catch (const std::exception &) {
      for (auto &&value : success)
        if (value)
          value = false;
    }
    for (std::size_t i = 0; i < success.size(); ++i)
      request.batch->per_key_results[request.start_idx + i] =
          success[i] ? 1 : 0;
  }
  void do_batch_set(SpdkConnectorConnection &,
                    const lmcache::connector::Request &request) override {
    const auto states = StatesFor(request.keys);
    const auto locks = LockStates(states);
    std::unordered_set<std::string> keys;
    std::vector<Reservation> reservations;
    std::vector<IoBatchItem> items;
    try {
      for (std::size_t i = 0; i < request.keys.size(); ++i) {
        if (!keys.insert(request.keys[i]).second)
          throw std::invalid_argument("batch store contains a duplicate key");
        if (request.buf_lens[i] == 0)
          throw std::invalid_argument("values must not be empty");
        const auto &current = states[i]->entry;
        if (current && current->logical_size != request.buf_lens[i])
          continue;
        reservations.push_back(
            {i, AllocateEntry(request.buf_lens[i]), current});
        AppendSegments(reservations.back().entry, request.buf_ptrs[i], i,
                       &items, nullptr);
      }
      if (!items.empty()) {
        // Submit all reserved objects before publishing any key, making this Store
        // future all-or-nothing even when each object spans multiple segments.
        const auto results = context_.write_batch(items);
        if (results.size() != items.size())
          throw std::runtime_error(
              "bdev write batch returned an unexpected result count");
        RequireSuccess(results, "bdev write batch");
      }
    } catch (...) {
      // Reservations are unpublished until all physical writes pass. Releasing
      // only these new extents leaves every prior key mapping untouched.
      for (const auto &reservation : reservations)
        allocator_.release(reservation.entry.extents);
      throw;
    }
    for (auto &reservation : reservations)
      states[reservation.index]->entry = std::move(reservation.entry);
    for (const auto &reservation : reservations)
      if (reservation.previous)
        allocator_.release(reservation.previous->extents);
  }
  bool do_single_exists(SpdkConnectorConnection &,
                        const std::string &key) override {
    const auto state = StateFor(key);
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->entry.has_value();
  }
  bool do_single_delete(SpdkConnectorConnection &,
                        const std::string &key) override {
    const auto state = StateFor(key);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->entry)
      return false;
    Entry entry = std::move(*state->entry);
    state->entry.reset();
    allocator_.release(entry.extents);
    return true;
  }
  void on_workers_stopped() override { context_.close(); }

private:
  struct Entry {
    std::uint64_t logical_size;
    std::vector<PageExtent> extents;
  };
  struct KeyState {
    std::mutex mutex;
    std::optional<Entry> entry;
  };
  struct Reservation {
    std::size_t index;
    Entry entry;
    std::optional<Entry> previous;
  };
  std::uint64_t PagesForBytes(std::uint64_t value) const {
    const auto page = context_.geometry().page_size;
    if (value > std::numeric_limits<std::uint64_t>::max() - (page - 1))
      throw std::overflow_error("value page count overflows uint64_t");
    return (value + page - 1) / page;
  }
  Entry AllocateEntry(std::uint64_t length) {
    return {length, allocator_.allocate(PagesForBytes(length))};
  }
  std::shared_ptr<KeyState> StateFor(const std::string &key) {
    std::lock_guard<std::mutex> lock(key_states_mutex_);
    auto &state = key_states_[key];
    if (!state)
      state = std::make_shared<KeyState>();
    return state;
  }
  std::vector<std::shared_ptr<KeyState>>
  StatesFor(const std::vector<std::string> &keys) {
    std::vector<std::shared_ptr<KeyState>> states;
    states.reserve(keys.size());
    for (const auto &key : keys)
      states.push_back(StateFor(key));
    return states;
  }
  static std::vector<std::unique_lock<std::mutex>>
  LockStates(const std::vector<std::shared_ptr<KeyState>> &states) {
    // Requests may repeat keys. Deduplicate then impose pointer order so two
    // overlapping multi-key requests acquire their per-key locks consistently.
    auto unique = states;
    std::sort(unique.begin(), unique.end(),
              [](const auto &left, const auto &right) {
                return std::less<KeyState *>{}(left.get(), right.get());
              });
    unique.erase(std::unique(unique.begin(), unique.end(),
                             [](const auto &left, const auto &right) {
                               return left.get() == right.get();
                             }),
                 unique.end());
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(unique.size());
    for (const auto &state : unique)
      locks.emplace_back(state->mutex);
    return locks;
  }
  void AppendSegments(const Entry &entry, void *buffer, std::size_t key,
                      std::vector<IoBatchItem> *items,
                      std::vector<std::size_t> *item_to_key) const {
    // Every allocated page must be transferred. Only the final physical range
    // can exceed logical bytes, and the staging write path zero-pads that tail.
    std::uint64_t logical = 0;
    for (const auto &extent : entry.extents) {
      std::uint64_t physical = 0;
      const auto bytes = extent.page_count * context_.geometry().page_size;
      while (physical < bytes) {
        const auto segment_physical =
            std::min(context_.geometry().io_segment_size, bytes - physical);
        const auto segment_logical =
            std::min(segment_physical, entry.logical_size - logical);
        if (segment_logical == 0)
          throw std::logic_error("entry extents exceed its logical size");
        items->push_back(
            {static_cast<char *>(buffer) + logical,
             static_cast<std::size_t>(segment_logical),
             extent.first_page * context_.geometry().page_size + physical,
             segment_physical,
             {}});
        if (item_to_key)
          item_to_key->push_back(key);
        logical += segment_logical;
        physical += segment_physical;
      }
    }
    if (logical != entry.logical_size)
      throw std::logic_error("entry extents do not cover its logical size");
  }
  void Write(const Entry &entry, const void *buffer) {
    std::vector<IoBatchItem> items;
    AppendSegments(entry, const_cast<void *>(buffer), 0, &items, nullptr);
    RequireSuccess(context_.write_batch(items), "bdev write");
  }
  void Read(const Entry &entry, void *buffer) {
    std::vector<IoBatchItem> items;
    AppendSegments(entry, buffer, 0, &items, nullptr);
    RequireSuccess(context_.read_batch(items), "bdev read");
  }
  SpdkBdevContext context_;
  PageExtentAllocator allocator_;
  std::mutex key_states_mutex_;
  std::vector<int> worker_cpus_;
  std::atomic<std::size_t> next_worker_cpu_{0};
  std::unordered_map<std::string, std::shared_ptr<KeyState>> key_states_;
};
} // namespace

void BindSpdkConnector(py::module_ &module) {
  py::class_<SpdkConnector>(module, "SpdkConnector")
      .def(py::init<const std::string &, const std::string &,
                    const std::vector<int> &, const std::vector<int> &,
                    std::size_t, std::size_t, int, bool, bool, std::uint64_t>(),
           py::arg("spdk_json_config"), py::arg("bdev_name"),
           py::arg("reactor_cpus"), py::arg("worker_cpus"),
           py::arg("spdk_io_thread_count"), py::arg("spdk_buffer_count"),
           py::arg("spdk_memory_size_mb"), py::arg("spdk_no_huge"),
           py::arg("spdk_no_pci"), py::arg("spdk_io_segment_bytes"))
      .def("runtime_status", &SpdkConnector::runtime_status)
          LMCACHE_BIND_CONNECTOR_METHODS(SpdkConnector);
}
} // namespace lmcache::spdk::pybind
