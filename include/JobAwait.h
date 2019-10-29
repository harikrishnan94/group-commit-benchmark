#pragma once

#include "types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

class JobAwait {
private:
  std::mutex wait_mtx;
  std::condition_variable wait_cv;
  std::atomic<bool> quit;
  std::atomic<bool> notified;

  bool awakened() { return quit || notified; }

public:
  enum AwaitStatus { AS_TIMEDOUT, AS_AWAKENED, AS_QUIT };

  void stop() {
    std::unique_lock<std::mutex> lock{wait_mtx};
    quit = true;
    wait_cv.notify_one();
  }

  void notify() {
    if (!notified.load(std::memory_order_relaxed) &&
        !quit.load(std::memory_order_relaxed)) {
      std::unique_lock<std::mutex> lock{wait_mtx};
      notified.store(true, std::memory_order_relaxed);
      wait_cv.notify_one();
    }
  }

  JobAwait() : wait_mtx(), wait_cv(), quit(false), notified(false) {}

  template <typename Duration> AwaitStatus await(Duration await_duration) {
    if (await_duration.count() && !awakened()) {
      std::unique_lock<std::mutex> lock{wait_mtx};
      wait_cv.wait_for(lock, await_duration,
                       [this]() { return this->awakened(); });
    }

    if (notified.load(std::memory_order_relaxed)) {
      notified.store(false, std::memory_order_relaxed);
      return AS_AWAKENED;
    }

    if (quit)
      return AS_QUIT;

    return AS_TIMEDOUT;
  }
};