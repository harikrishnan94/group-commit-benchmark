#pragma once

#include "JobAwait.h"
#include "filedesc.h"
#include "types.h"

#include <array>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <string_view>
#include <vector>

#include <boost/circular_buffer.hpp>
#include <boost/scope_exit.hpp>

using namespace std::chrono_literals;

using log_clock = std::chrono::system_clock;
using time_point = log_clock::time_point;

struct alignas(128) WaitNode {
  std::mutex m;
  std::condition_variable c;
};

class LogRec {
public:
  LogRec(std::unique_ptr<char[]> data, usize size)
      : data(std::move(data)), size(size) {}

  LogRec(const LogRec &) = delete;
  LogRec(LogRec &&) = default;

private:
  friend class LogFlusher;

  std::unique_ptr<char[]> data;
  usize size;
  std::chrono::nanoseconds enque_dur;
};

class LogFlusher {
private:
  using LogList = std::vector<LogRec *>;
  static constexpr usize LOG_FILE_PRE_ALLOCATE_SIZE = 16 * 1024 * 1024;
  static constexpr auto MAX_WAIT_DURATION = 50000us;

  filedesc m_fd;
  usize m_total_log_size = 0;
  usize m_log_file_size = 0;
  std::atomic<usize> m_batch_size = 0;
  LogList m_batch = {};
  usize m_current_batch_size = 0;
  std::mutex m_log_mtx = {};
  usize m_lsn = 0;
  std::atomic<usize> m_flushed_lsn = 0;
  usize m_num_flushes = 0;
  std::vector<std::chrono::nanoseconds> m_enque_durations = {};
  std::vector<std::chrono::nanoseconds> m_deque_durations = {};
  std::vector<std::chrono::nanoseconds> m_wakeup_durations = {};
  std::vector<std::chrono::nanoseconds> m_flush_durations = {};
  std::vector<usize> m_flush_sizes = {};
  std::array<WaitNode, 128> m_wait_vars;
  JobAwait m_flush_await = {};
  JobAwait m_notify_await = {};

  auto get_flush_batch() -> std::tuple<LogList, usize, usize> {
    auto start = std::chrono::steady_clock::now();
    BOOST_SCOPE_EXIT(start, this_) {
      this_->m_deque_durations.emplace_back(std::chrono::steady_clock::now() -
                                            start);
    }
    BOOST_SCOPE_EXIT_END;

    std::lock_guard lock{m_log_mtx};
    m_current_batch_size = 0;
    return {std::exchange(m_batch, {}), m_lsn, m_total_log_size};
  }

  void extend_logfile(usize total_log_size) {
    if (m_log_file_size <= total_log_size) {
      m_log_file_size = std::max(m_log_file_size + LOG_FILE_PRE_ALLOCATE_SIZE,
                                 total_log_size);

      if (m_fd.truncate(m_log_file_size)) {
        throw std::system_error(errno, std::generic_category());
      }
    }
  }

  auto get_io_buffers(const LogList &logs)
      -> std::pair<std::vector<struct iovec>, usize> {
    std::vector<struct iovec> write_buffers;
    usize flush_size = 0;

    write_buffers.reserve(logs.size());
    for (auto log : logs) {
      write_buffers.push_back({(void *)log->data.get(), log->size});
      flush_size += log->size;
      m_enque_durations.push_back(log->enque_dur);
    }

    return {write_buffers, flush_size};
  }

  microseconds flush_batch(microseconds wait_duration) {
    auto start = std::chrono::steady_clock::now();
    auto [logs, lsn, total_log_size] = get_flush_batch();

    if (logs.empty())
      return std::clamp(wait_duration * 2, 1us, MAX_WAIT_DURATION);

    extend_logfile(total_log_size);

    auto [write_buffers, flush_size] = get_io_buffers(logs);

    if (m_fd.write(write_buffers.data(), write_buffers.size()))
      throw std::system_error(errno, std::generic_category());

    m_flushed_lsn = lsn;
    m_notify_await.notify();

    m_num_flushes++;
    m_flush_durations.emplace_back(std::chrono::steady_clock::now() - start);
    m_flush_sizes.push_back(flush_size);
    return 0us;
  }

  void await_flush(usize lsn) {
    auto is_flushed = [&]() { return m_flushed_lsn >= lsn; };
    auto &wait_var =
        m_wait_vars[std::hash<std::thread::id>{}(std::this_thread::get_id()) %
                    m_wait_vars.size()];
    std::unique_lock l{wait_var.m};
    wait_var.c.wait(l, is_flushed);
  }

  void wakeup_waiters() {
    auto start = std::chrono::steady_clock::now();
    for (auto &waitvar : m_wait_vars) {
      waitvar.c.notify_all();
    }
    m_wakeup_durations.emplace_back(std::chrono::steady_clock::now() - start);
  }

public:
  LogFlusher(filedesc &&a_fd) : m_fd(std::move(a_fd)) {}

  void start() {
    std::thread notifier{[this]() {
      while (m_notify_await.await(MAX_WAIT_DURATION) != JobAwait::AS_QUIT) {
        wakeup_waiters();
      }
    }};
    auto wait_duration = MAX_WAIT_DURATION;
    while (m_flush_await.await(wait_duration) != JobAwait::AS_QUIT) {
      wait_duration = flush_batch(wait_duration);
    }
    notifier.join();
  }

  void stop() {
    m_notify_await.stop();
    m_flush_await.stop();
  }

  usize get_num_flushes() const { return m_num_flushes; }
  const std::vector<std::chrono::nanoseconds> &get_wakeup_durations() const
      noexcept {
    return m_wakeup_durations;
  }
  const std::vector<std::chrono::nanoseconds> &get_flush_durations() const
      noexcept {
    return m_flush_durations;
  }
  const std::vector<std::chrono::nanoseconds> &get_enque_durations() const
      noexcept {
    return m_enque_durations;
  }
  const std::vector<std::chrono::nanoseconds> &get_deque_durations() const
      noexcept {
    return m_deque_durations;
  }
  const std::vector<usize> &get_flush_sizes() const noexcept {
    return m_flush_sizes;
  }

  void sync_flush(LogRec &log) {
    auto start = std::chrono::steady_clock::now();
    usize batch_size;
    usize lsn;
    {
      std::lock_guard lock{m_log_mtx};
      auto size = log.size;

      m_batch.push_back(&log);
      batch_size = m_current_batch_size += size;
      m_total_log_size += size;
      lsn = ++m_lsn;
    }
    log.enque_dur = std::chrono::steady_clock::now() - start;

    await_flush(lsn);
  }
};