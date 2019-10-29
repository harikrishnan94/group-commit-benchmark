#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#include <boost/program_options.hpp>

#include "LogFlusher.h"
#include "chrono2string.h"
#include "types.h"

struct BMArgs {
  u32 num_threads;
  u32 num_txns_per_thread;
  u32 txn_size;
};
using micros = std::chrono::microseconds;

constexpr u32 MAX_TXN_SIZE = 128 * 1024;

static std::string format_to_si(double size /*in bytes*/);
template <typename Duration>
static void print_latencies(std::string_view name,
                            const std::vector<Duration> &times,
                            int num_percentiles = 10);
static void print_size_stats(std::string_view name,
                             const std::vector<usize> &sizes_,
                             int num_percentiles = 10);
class Session {
  const u32 num_txns_per_thread;
  const u32 txn_size;
  LogFlusher &flusher;

  std::vector<micros> latencies;

public:
  Session(u32 a_num_txns_per_thread, u32 a_txn_size, LogFlusher &a_flusher)
      : num_txns_per_thread(a_num_txns_per_thread), txn_size(a_txn_size),
        flusher(a_flusher), latencies() {}

  void process() {
    auto gen_data =
        [gen = std::mt19937{std::random_device{}()},
         dis = std::uniform_int_distribution<char>{}](auto txn_size) mutable {
          auto data = std::make_unique<char[]>(txn_size);
          std::generate(data.get(), data.get() + txn_size,
                        [&dis, &gen]() { return dis(gen); });

          return data;
        };

    for (u32 i = 0; i < num_txns_per_thread; i++) {
      auto start = log_clock::now();
      {
        auto data = gen_data(txn_size);
        auto log = LogRec(std::move(data), txn_size);

        flusher.sync_flush(log);
      }
      auto end = std::chrono::system_clock::now();
      latencies.push_back(std::chrono::duration_cast<micros>(end - start));
    }
  }

  const std::vector<micros> &get_txn_latencies() const { return latencies; }
};

static int do_benchmark(BMArgs args) {
  try {
    LogFlusher logger{filedesc::open_in_temp(
        "bm_logger", O_DSYNC | O_WRONLY | O_CREAT | O_EXCL)};
    std::thread flusher{[&logger]() { logger.start(); }};

    std::vector<Session> sessions;
    std::vector<std::thread> txn_workers;
    std::vector<micros> txn_latencies;

    sessions.reserve(args.num_threads);
    txn_workers.reserve(args.num_threads);

    auto start = std::chrono::system_clock::now();

    for (int i = 0; i < args.num_threads; i++) {
      sessions.emplace_back(args.num_txns_per_thread, args.txn_size,
                            std::ref(logger));
      txn_workers.emplace_back(
          [session = &sessions[i]]() { session->process(); });
    }

    for (auto &worker : txn_workers) {
      worker.join();
    }

    auto end = std::chrono::system_clock::now();
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    logger.stop();
    flusher.join();

    u64 num_txns =
        static_cast<u64>(args.num_txns_per_thread) * args.num_threads;

    std::cout << "Time taken to Execute " << num_txns
              << " Transactions = " << std::to_string(elapsed_ms) << " ";

    if (elapsed_ms.count()) {
      std::cout << "(" << std::setw(2)
                << format_to_si(num_txns / (elapsed_ms.count() / 1000.0))
                << "Txn/s, "
                << format_to_si((num_txns * args.txn_size) /
                                (elapsed_ms.count() / 1000.0))
                << "B/s)\n";
    } else {
      std::cout << "\n";
    }

    auto num_flushes = logger.get_num_flushes();
    std::cout << "# Flushes = " << num_flushes << " ("
              << ((num_flushes * 1000) / elapsed_ms.count()) << "/s)\n";

    txn_latencies.reserve(num_txns);

    for (const auto &session : sessions) {
      auto &latencies = session.get_txn_latencies();
      txn_latencies.insert(txn_latencies.end(), latencies.begin(),
                           latencies.end());
    }

    print_latencies("Transaction", txn_latencies);
    print_latencies("Enque", logger.get_enque_durations());
    print_latencies("Deque", logger.get_deque_durations());
    print_size_stats("Flush", logger.get_flush_sizes());
    print_latencies("Flush", logger.get_flush_durations());
    print_latencies("Wakeup", logger.get_wakeup_durations());
    return 0;
  } catch (std::exception &e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
  }

  return 1;
}

template <typename T> static T calc_sum(const std::vector<T> &times) {
  return std::accumulate(times.begin(), times.end(), T{0});
}

template <typename T> static T calc_mean(const std::vector<T> &times) {
  return calc_sum(times) / times.size();
}

template <typename Duration, typename Int>
Int operator/(Duration a, Int b) noexcept {
  return a.count() / b;
}

template <typename Rep, typename Period, typename Int>
Int div(std::chrono::duration<Rep, Period> a, Int b) noexcept {
  return a.count() / b;
}
template <typename Rep, typename Period>
auto mul(std::chrono::duration<Rep, Period> a,
         std::chrono::duration<Rep, Period> b) noexcept {
  return std::chrono::duration<Rep, Period>{a.count() * b.count()};
}
template <typename Int, typename = std::enable_if_t<std::is_integral_v<Int>>>
Int div(Int a, Int b) noexcept {
  return a / b;
}
template <typename Int, typename = std::enable_if_t<std::is_integral_v<Int>>>
Int mul(Int a, Int b) noexcept {
  return a * b;
}

template <typename T> static T calc_stddev(const std::vector<T> &times) {
  std::vector<T> diff(times.size());
  auto avg = calc_mean(times);
  std::transform(times.begin(), times.end(), diff.begin(),
                 [avg](auto x) { return x - avg; });
  auto sq_sum = std::inner_product(diff.begin(), diff.end(), diff.begin(), T{0},
                                   std::plus<T>(),
                                   [](auto x, auto y) { return mul(x, y); });
  return T{static_cast<u64>(std::sqrt(div(sq_sum, times.size())))};
}

template <typename T>
static auto calc_percentiles(const std::vector<T> &times, int num_percentiles) {
  std::vector<std::pair<int, T>> percentiles;

  auto size = times.size();
  int delta = 100 / num_percentiles;
  num_percentiles = 100 / delta;

  for (int i = 1; i < num_percentiles; i++) {
    percentiles.emplace_back(i * delta,
                             times[(size / num_percentiles) * i - 1]);
  }

  percentiles.emplace_back(99, times[(size * 99) / 100 - 1]);
  return percentiles;
}

template <typename Duration>
static void print_latencies(std::string_view name,
                            const std::vector<Duration> &times,
                            int num_percentiles) {
  if (!times.empty()) {
    auto lats = times;
    std::sort(lats.begin(), lats.end());

    auto mean = calc_mean(lats);
    auto stddev = calc_stddev(lats);
    auto min = lats.front();
    auto max = lats.back();
    auto percentiles = calc_percentiles(lats, num_percentiles);

    std::cout << name << " latency statistics: ";
    std::cout << "min=" << std::to_string(min) << " ";
    std::cout << "max=" << std::to_string(max) << " ";
    std::cout << "mean=" << std::to_string(mean) << " ";
    std::cout << "stddev=" << std::to_string(stddev) << "\n";
    std::cout << name << " latency percentiles: ";

    for (auto [pctile, lat] : percentiles) {
      std::cout << pctile << "th=" << std::to_string(lat) << ", ";
    }

    std::cout << "\n";
  }
}

static void print_size_stats(std::string_view name,
                             const std::vector<usize> &sizes_,
                             int num_percentiles) {
  if (!sizes_.empty()) {
    auto sizes = sizes_;
    std::sort(sizes.begin(), sizes.end());

    auto mean = calc_mean(sizes);
    auto stddev = calc_stddev(sizes);
    auto min = sizes.front();
    auto max = sizes.back();
    auto percentiles = calc_percentiles(sizes, num_percentiles);

    std::cout << name << " size statistics: ";
    std::cout << "min=" << format_to_si(min) << " ";
    std::cout << "max=" << format_to_si(max) << " ";
    std::cout << "mean=" << format_to_si(mean) << " ";
    std::cout << "stddev=" << format_to_si(stddev) << "\n";
    std::cout << name << " size percentiles: ";

    for (auto [pctile, lat] : percentiles) {
      std::cout << pctile << "th=" << format_to_si(lat) << ", ";
    }

    std::cout << "\n";
  }
}

int main(int argc, char *argv[]) {
  namespace po = boost::program_options;

  po::options_description options{"Transaction Group Flush Benchmark"};

  options.add_options()("help,h", "Display this help message");

  options.add_options()("txns,x", po::value<u32>()->required(),
                        "# Xact per thread");
  options.add_options()("txnsize,s", po::value<u32>()->required(), "Xact Size");
  options.add_options()("threads,t", po::value<u32>()->required(),
                        "# threads to use for benchmark");

  BMArgs args;

  try {
    po::variables_map vm;

    po::store(po::command_line_parser(argc, argv).options(options).run(), vm);
    po::notify(vm);

    args.num_threads = vm["threads"].as<u32>();
    args.num_txns_per_thread = vm["txns"].as<u32>();
    args.txn_size = vm["txnsize"].as<u32>();

    if (args.txn_size > MAX_TXN_SIZE) {
      throw std::string{"Xact size is too big"};
    }

    return do_benchmark(args);
  } catch (const po::error &ex) {
    std::cerr << "ERROR: " << ex.what() << std::endl;
    std::cerr << options << std::endl;
  } catch (...) {
    std::cerr << options << std::endl;
  }

  return -1;
}

static std::string format_to_si(double size /*in bytes*/) {
  char buf[100];

  int i = 0;
  const char *units[] = {"", "K", "M", "G", "T", "P", "E", "Z", "Y"};
  while (size > 1024) {
    size /= 1024;
    i++;
  }

  sprintf(buf, "%.*lf %s", i, size, units[i]);
  return std::string{buf};
}
