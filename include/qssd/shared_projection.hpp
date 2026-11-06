#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace qssd {
enum class ProjectionPort { CPU, SSD, GPU };

// C02: a typed observation of an existing authority, shared by identity/version.
// The producer supplies the original value. qSSD adds no geometry, physics,
// quantum measurement, approximation or per-consumer dense data array.
template<class Value, class Equal = std::equal_to<Value>>
class SharedProjection {
 public:
  struct Lease {
    std::uint64_t identity = 0, revision = 0;
    std::shared_ptr<const Value> value;
    explicit operator bool() const { return bool(value); }
  };
  struct Stats { std::uint64_t publications = 0, unchanged = 0, acquisitions = 0; };
  Lease publish(Value value) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (current_.value && Equal{}(*current_.value, value)) { ++stats_.unchanged; return current_; }
    if (current_.revision == INT64_MAX) throw std::overflow_error("qSSD projection revision exhausted");
    Lease next{identity_, current_.revision + 1, std::make_shared<const Value>(std::move(value))};
    current_ = std::move(next); ++stats_.publications; return current_;
  }
  Lease acquire(ProjectionPort port) const {
    if (port != ProjectionPort::CPU && port != ProjectionPort::SSD && port != ProjectionPort::GPU)
      throw std::invalid_argument("unknown qSSD projection port");
    std::lock_guard<std::mutex> guard(mutex_);
    if (!current_) throw std::logic_error("qSSD projection has not been published");
    ++stats_.acquisitions; return current_;
  }
  void reset() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (identity_ == INT64_MAX) throw std::overflow_error("qSSD projection identity exhausted");
    ++identity_; current_ = {};
  }
  Stats stats() const { std::lock_guard<std::mutex> guard(mutex_); return stats_; }
 private:
  mutable std::mutex mutex_;
  std::uint64_t identity_ = 1;
  Lease current_;
  mutable Stats stats_;
};
}  // namespace qssd
