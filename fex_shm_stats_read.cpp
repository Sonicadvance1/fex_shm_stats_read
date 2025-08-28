// SPDX-License-Identifier: MIT
// clang++ -static -fuse-ld=lld -g -o fex_shm_stats_read fex_shm_stats_read.cpp -std=c++20 `pkgconf --libs --static ncursesw`
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <fcntl.h>
#include <locale.h>
#include <map>
#include <ranges>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <thread>
#include <unistd.h>
#include <ncurses.h>
#include <signal.h>
#include <vector>

// Reimplementation of FEXCore::Profiler struct types.
namespace FEXCore::Profiler {
constexpr uint32_t STATS_VERSION = 2;
enum class AppType : uint8_t {
  LINUX_32,
  LINUX_64,
  WIN_ARM64EC,
  WIN_WOW64,
};

struct ThreadStatsHeader {
  uint8_t Version;
  AppType app_type;
  uint8_t _pad[2];
  char fex_version[48];
  std::atomic<uint32_t> Head;
  std::atomic<uint32_t> Size;
  uint32_t pad;
};

struct ThreadStats {
  uint32_t Next;
  uint32_t TID;

  // Accumulated time
  uint64_t AccumulatedJITTime;
  uint64_t AccumulatedSignalTime;

  // Accumulated event counts
  uint64_t SIGBUSCount;
  uint64_t SMCCount;
  uint64_t FloatFallbackCount;
};
} // namespace FEXCore::Profiler

static const char* GetAppType(FEXCore::Profiler::AppType Type) {
  switch (Type) {
  case FEXCore::Profiler::AppType::LINUX_32: return "Linux32";
  case FEXCore::Profiler::AppType::LINUX_64: return "Linux64";
  case FEXCore::Profiler::AppType::WIN_ARM64EC: return "arm64ec";
  case FEXCore::Profiler::AppType::WIN_WOW64: return "wow64";
  default: break;
  }

  return "Unknown";
}


static const std::array<wchar_t, 10> partial_pips {
  L'\U00002002', // 0%: Empty
  L'\U00002581', // 10%: 1/8 (12.5%)
  L'\U00002581', // 20%: 1/8 (12.5%)
  L'\U00002582', // 30%: 2/8 (25%)
  L'\U00002583', // 40%: 3/8 (37.5%)
  L'\U00002584', // 50%: 4/8 (50%)
  L'\U00002585', // 60%: 5/8 (62.5%)
  L'\U00002586', // 70%: 6/8 (75%)
  L'\U00002587', // 80%: 7/8 (87.5%)
  L'\U00002588', // Full
};

struct fex_stats {
  int pid {-1};
  int shm_fd {-1};
  bool first_sample = true;
  uint32_t shm_size {};
  uint64_t cycle_counter_frequency {};
  size_t hardware_concurrency {};
  size_t page_size {};

  void* shm_base {};
  FEXCore::Profiler::ThreadStatsHeader* head {};
  FEXCore::Profiler::ThreadStats* stats {};

  struct retained_stats {
    std::chrono::time_point<std::chrono::steady_clock> LastSeen;
    FEXCore::Profiler::ThreadStats PreviousStats {};
    FEXCore::Profiler::ThreadStats Stats {};
  };

  std::chrono::time_point<std::chrono::steady_clock> previous_sample_period;
  std::map<uint32_t, retained_stats> sampled_stats;

  std::wstring empty_pip_data;

  struct max_thread_loads {
    float load_percentage {};
    std::wstring pip_data {};
  };
  std::vector<max_thread_loads> max_thread_loads {};

  std::vector<float> fex_load_histogram;

  int pidfd_watch {-1};

  fex_stats()
    : fex_load_histogram(200, 0.0f) {}
};

fex_stats g_stats {};

#ifndef __x86_64__
uint64_t get_cycle_counter_frequency() {
  uint64_t result;
  __asm("mrs %[Res], CNTFRQ_EL0;\n" : [Res] "=r"(result));

  return result;
}
static void store_memory_barrier() {
  asm volatile("dmb ishst" ::: "memory");
}
#else
static uint64_t get_cycle_counter_frequency() {
  return 1;
}
static void store_memory_barrier() {}
#endif

static FEXCore::Profiler::ThreadStats* StatFromOffset(void* Base, uint32_t Offset) {
  return reinterpret_cast<FEXCore::Profiler::ThreadStats*>(reinterpret_cast<uint64_t>(Base) + Offset);
}

static void exit_screen(const char* format = nullptr, ...) {
  refresh();
  endwin();

  if (format != nullptr) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
  }
  _exit(0);
}

static void handle_signal(int signum, siginfo_t* info, void* context) {
  exit_screen();
}

static void setup_signal_handler() {
  struct sigaction sa;
  sa.sa_sigaction = handle_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_SIGINFO;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);
}

static void check_shm_update_necessary() {
  auto new_shm_size = g_stats.head->Size.load(std::memory_order_relaxed);
  if (g_stats.shm_size != new_shm_size) {
    // Remap!
    munmap(g_stats.shm_base, g_stats.shm_size);
    g_stats.shm_size = new_shm_size;
    g_stats.shm_base = mmap(nullptr, new_shm_size, PROT_READ, MAP_SHARED, g_stats.shm_fd, 0);
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: %s [options] <pid>\n", argv[0]);
    return 0;
  }

  setup_signal_handler();

  const auto fex_shm = std::format("fex-{}-stats", argv[argc - 1]);
  g_stats.shm_fd = shm_open(fex_shm.c_str(), O_RDONLY, 0);
  if (g_stats.shm_fd == -1) {
    printf("%s doesn't seem to exist\n", fex_shm.c_str());
    return 1;
  }

  struct stat buf {};
  if (fstat(g_stats.shm_fd, &buf) == -1) {
    printf("Couldn't stat\n");
    return 1;
  }

  if (buf.st_size < sizeof(uint64_t) * 4) {
    printf("Buffer was too small: %ld\n", buf.st_size);
    return 1;
  }

  g_stats.pidfd_watch = ::syscall(SYS_pidfd_open, strtol(argv[argc - 1], nullptr, 10), 0);
  setlocale(LC_ALL, "");
  initscr();
  start_color();
  init_pair(1, COLOR_RED, COLOR_BLACK);
  init_pair(2, COLOR_YELLOW, COLOR_BLACK);

  g_stats.shm_size = buf.st_size;
  g_stats.shm_base = mmap(nullptr, g_stats.shm_size, PROT_READ, MAP_SHARED, g_stats.shm_fd, 0);
  g_stats.head = reinterpret_cast<FEXCore::Profiler::ThreadStatsHeader*>(g_stats.shm_base);

  std::string fex_version {g_stats.head->fex_version, strnlen(g_stats.head->fex_version, sizeof(g_stats.head->fex_version))};

  store_memory_barrier();
  printw("Header for PID %s:\n", argv[argc - 1]);
  printw("  Version: 0x%x\n", g_stats.head->Version);
  printw("  Type: %s\n", GetAppType(g_stats.head->app_type));
  printw("  Fex: %s\n", fex_version.c_str());
  printw("  Head: 0x%x\n", g_stats.head->Head.load(std::memory_order_relaxed));
  printw("  Size: 0x%x\n", g_stats.head->Size.load(std::memory_order_relaxed));

  if (g_stats.head->Version != FEXCore::Profiler::STATS_VERSION) {
    exit_screen("Unhandled FEX stats version\n");
  }

  g_stats.cycle_counter_frequency = get_cycle_counter_frequency();
  g_stats.hardware_concurrency = std::thread::hardware_concurrency();
  g_stats.max_thread_loads.reserve(g_stats.hardware_concurrency);

  bool FirstLoop = true;
  double Scale = 1000.0;
  const char* ScaleStr = "ms/second";
  const auto SamplePeriod = std::chrono::seconds(1);

  while (true) {
    if (g_stats.pidfd_watch != -1) {
      pollfd fd {
        .fd = g_stats.pidfd_watch,
        .events = POLLIN | POLLHUP,
        .revents = 0,
      };
      int Res = poll(&fd, 1, 0);
      if (Res == 1) {
        if (fd.revents & POLLHUP) {
          exit_screen("FEX process exited\n");
        }
      }
    }

    FEXCore::Profiler::ThreadStats TotalThisPeriod {};

    // The writer side doesn't use atomics. Use a memory barrier to ensure writes are visible.
    store_memory_barrier();

    check_shm_update_necessary();

    uint32_t HeaderOffset = g_stats.head->Head;

    auto Now = std::chrono::steady_clock::now();
    while (HeaderOffset != 0) {
      if (HeaderOffset >= g_stats.shm_size) {
        break;
      }
      FEXCore::Profiler::ThreadStats* Stat = StatFromOffset(g_stats.shm_base, HeaderOffset);

      auto it = &g_stats.sampled_stats[Stat->TID];
      memcpy(&it->PreviousStats, &it->Stats, sizeof(FEXCore::Profiler::ThreadStats));
      memcpy(&it->Stats, Stat, sizeof(FEXCore::Profiler::ThreadStats));
      it->LastSeen = Now;

      HeaderOffset = Stat->Next;
    }

    uint64_t total_jit_time {};
    size_t threads_sampled {};
    std::vector<uint64_t> hottest_threads;
#define accumulate(dest, name) dest += Stat->name - PreviousStats->name
    for (auto it = g_stats.sampled_stats.begin(); it != g_stats.sampled_stats.end();) {
      ++threads_sampled;
      auto PreviousStats = &it->second.PreviousStats;
      auto Stat = &it->second.Stats;
      uint64_t total_time {};

      accumulate(total_time, AccumulatedJITTime);
      accumulate(total_time, AccumulatedSignalTime);
      total_jit_time += total_time;

      accumulate(TotalThisPeriod.AccumulatedJITTime, AccumulatedJITTime);
      accumulate(TotalThisPeriod.AccumulatedSignalTime, AccumulatedSignalTime);

      accumulate(TotalThisPeriod.SIGBUSCount, SIGBUSCount);
      accumulate(TotalThisPeriod.SMCCount, SMCCount);
      accumulate(TotalThisPeriod.FloatFallbackCount, FloatFallbackCount);

      memcpy(PreviousStats, Stat, sizeof(FEXCore::Profiler::ThreadStats));

      if ((Now - it->second.LastSeen) >= std::chrono::seconds(10)) {
        it = g_stats.sampled_stats.erase(it);
        continue;
      }

      hottest_threads.emplace_back(total_time);

      ++it;
    }

    std::sort(hottest_threads.begin(), hottest_threads.end(), std::greater<uint64_t>());

    // Calculate loads based on the sample period that occurred.
    // FEX-Emu only counts cycles for the amount of time, so we need to calculate load based on the number of cycles that the sample period has.
    const auto sample_period = Now - g_stats.previous_sample_period;

    const double NanosecondsInSeconds = 1'000'000'000.0;
    const double SamplePeriodNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(sample_period).count();
    const double MaximumCyclesInSecond = (double)g_stats.cycle_counter_frequency;
    const double MaximumCyclesInSamplePeriod = MaximumCyclesInSecond * (SamplePeriodNanoseconds / NanosecondsInSeconds);
    const double MaximumCoresThreadsPossible = std::min(g_stats.hardware_concurrency, threads_sampled);

    double fex_load = ((double)total_jit_time / (MaximumCyclesInSamplePeriod * MaximumCoresThreadsPossible)) * 100.0;
    size_t minimum_hot_threads = std::min(g_stats.hardware_concurrency, hottest_threads.size());
    // For the top thread-loads, we are only ever showing up to how many hardware threads are available.
    g_stats.max_thread_loads.resize(minimum_hot_threads);
    for (size_t i = 0; i < minimum_hot_threads; ++i) {
      g_stats.max_thread_loads[i].load_percentage = ((double)hottest_threads[i] / MaximumCyclesInSamplePeriod) * 100.0;
    }

    const size_t HistogramHeight = 11;
    if (!FirstLoop) {
      const auto JITSeconds = (double)(TotalThisPeriod.AccumulatedJITTime) / (double)g_stats.cycle_counter_frequency;
      const auto SignalTime = (double)(TotalThisPeriod.AccumulatedSignalTime) / (double)g_stats.cycle_counter_frequency;

      const auto SIGBUSCount = TotalThisPeriod.SIGBUSCount;
      const auto SMCCount = TotalThisPeriod.SMCCount;
      const auto FloatFallbackCount = TotalThisPeriod.FloatFallbackCount;

      const auto MaxActiveThreads = std::min<size_t>(g_stats.sampled_stats.size(), g_stats.hardware_concurrency);

      mvprintw(LINES - 7 - minimum_hot_threads - HistogramHeight, 0, "%ld threads executing\n", minimum_hot_threads);

      size_t max_pips = std::min(COLS, 50) - 2;
      double percentage_per_pip = 100.0 / (double)max_pips;

      g_stats.empty_pip_data.resize(max_pips);
      std::fill(g_stats.empty_pip_data.begin(), g_stats.empty_pip_data.begin() + max_pips, partial_pips.front());
      for (size_t i = 0; i < g_stats.max_thread_loads.size(); ++i) {
        auto& thread_loads = g_stats.max_thread_loads[i];
        double thread_load = std::min(thread_loads.load_percentage, 100.0f);
        thread_loads.pip_data.resize(max_pips + 1);
        double rounded_down = std::floor(thread_load / 10.0) * 10.0;
        size_t full_pips = rounded_down / percentage_per_pip;
        size_t digit_percent = thread_load - rounded_down;
        wmemset(thread_loads.pip_data.data(), partial_pips.front(), thread_loads.pip_data.size());
        wmemset(thread_loads.pip_data.data(), partial_pips.back(), full_pips);
        wmemset(thread_loads.pip_data.data() + full_pips, partial_pips[digit_percent], 1);

        const auto y_offset = LINES - 7 - i - HistogramHeight;
        mvprintw(y_offset, 0, "[%ls]: %.02f%%\n", g_stats.empty_pip_data.data(), thread_load);
        int attr = 0;
        if (thread_load >= 75.0) {
          attr = 1;
        } else if (thread_load >= 50.0) {
          attr = 2;
        }
        if (attr) {
          attron(COLOR_PAIR(attr));
        }
        mvprintw(y_offset, 1, "%ls", thread_loads.pip_data.data());
        if (attr) {
          attroff(COLOR_PAIR(attr));
        }
      }

      mvprintw(LINES - 6 - HistogramHeight, 0, "Total (%zd millisecond sample period):\n",
               std::chrono::duration_cast<std::chrono::milliseconds>(SamplePeriod).count());
      mvprintw(LINES - 5 - HistogramHeight, 0, "       JIT Time: %f %s (%.2f percent)\n", JITSeconds * Scale, ScaleStr,
               JITSeconds / (double)MaxActiveThreads * 100.0);
      mvprintw(LINES - 4 - HistogramHeight, 0, "    Signal Time: %f %s (%.2f percent)\n", SignalTime * Scale, ScaleStr,
               SignalTime / (double)MaxActiveThreads * 100.0);

      double SIGBUS_l = SIGBUSCount;
      double SIGBUS_Per_Second = SIGBUS_l * (SamplePeriodNanoseconds / NanosecondsInSeconds);
      mvprintw(LINES - 3 - HistogramHeight, 0, "     SIGBUS Cnt: %ld (%lf per second)\n", SIGBUSCount, SIGBUS_Per_Second);
      mvprintw(LINES - 2 - HistogramHeight, 0, "        SMC Cnt: %ld\n", SMCCount);
      mvprintw(LINES - 1 - HistogramHeight, 0, "  Softfloat Cnt: %ld\n", FloatFallbackCount);
    }

    g_stats.fex_load_histogram.erase(g_stats.fex_load_histogram.begin());
    g_stats.fex_load_histogram.push_back(fex_load);

    size_t HistogramWidth = COLS - 2;
    for (size_t i = 0; i < HistogramHeight - 1; ++i) {
      mvprintw(LINES - i - 1, 0, "[");
      mvprintw(LINES - i - 1, COLS - 1, "]");
    }

    mvprintw(LINES - HistogramHeight, 0, "FEX JIT Load: %f (cycles: %ld)\n", fex_load, total_jit_time);
    size_t j = 0;
    for (auto& HistogramResult : std::ranges::reverse_view {g_stats.fex_load_histogram}) {
      for (size_t i = 0; i < HistogramHeight - 1; ++i) {
        int attr = 0;
        if (HistogramResult >= 75.0) {
          attr = 1;
        } else if (HistogramResult >= 50.0) {
          attr = 2;
        }
        if (attr) {
          attron(COLOR_PAIR(attr));
        }

        double rounded_down = std::floor(HistogramResult / 10.0) * 10.0;
        size_t tens_digit = rounded_down / 10.0;
        size_t digit_percent = std::floor(HistogramResult - rounded_down);

        size_t pip = 0;
        if (tens_digit > i) {
          pip = partial_pips.size() - 1;
        } else if (tens_digit == i) {
          pip = digit_percent;
        }

        mvprintw(LINES - i - 1, HistogramWidth - j, "%lc", partial_pips[pip]);
        if (attr) {
          attroff(COLOR_PAIR(attr));
        }
      }
      ++j;
      if (j >= HistogramWidth) {
        break;
      }
    }

    FirstLoop = false;

    g_stats.previous_sample_period = Now;

    refresh();
    std::this_thread::sleep_for(SamplePeriod);
  }

  close(g_stats.shm_fd);
  close(g_stats.pidfd_watch);
  exit_screen();
  return 0;
}
