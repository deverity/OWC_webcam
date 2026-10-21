#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace owc {

void setThreadName(const char* name);  // <= 15 chars
// Tries `nice` values from `wanted` towards 0; returns the value applied (0 if none).
int raiseThreadPriority(int wanted);

// Unbounded MPSC queue with timed pop, used for the worker event loop.
template <class T>
class EventQueue {
 public:
  void push(T v) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      q_.push_back(std::move(v));
    }
    cv_.notify_one();
  }
  // Waits until an item is available or `deadline` passes.
  std::optional<T> popUntil(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_until(lk, deadline, [&] { return !q_.empty(); });
    if (q_.empty()) return std::nullopt;
    T v = std::move(q_.front());
    q_.pop_front();
    return v;
  }
  void clear() {
    std::lock_guard<std::mutex> lk(mu_);
    q_.clear();
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<T> q_;
};

}  // namespace owc
