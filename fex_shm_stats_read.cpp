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
  const char *pid_str {};
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

  struct FEXMemStats final {
    // Total resident
    std::atomic<uint64_t> TotalAnon {~0ULL};

    // JIT Code
    std::atomic<uint64_t> JITCode {~0ULL};
    std::atomic<uint64_t> OpDispatcher {~0ULL};
    std::atomic<uint64_t> Frontend {~0ULL};
    std::atomic<uint64_t> CPUBackend {~0ULL};
    std::atomic<uint64_t> Lookup {~0ULL};
    std::atomic<uint64_t> LookupL1 {~0ULL};
    std::atomic<uint64_t> ThreadStates {~0ULL};
    std::atomic<uint64_t> BlockLinks {~0ULL};
    std::atomic<uint64_t> Misc {~0ULL};
    std::atomic<uint64_t> JEMalloc {~0ULL};
    std::atomic<uint64_t> Unaccounted {~0ULL};

    struct LargestAnonType {
      uint64_t Begin, End;
      uint64_t Size;
    };
    LargestAnonType LargestAnon;
  };
  FEXMemStats MemStats;

  std::atomic<bool> ShuttingDown {};

  int pidfd_watch {-1};

  fex_stats()
    : fex_load_histogram(200, 0.0f) {}
};

const auto SamplePeriod = std::chrono::seconds(1);
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

    // Update head pointer as well.
    g_stats.head = reinterpret_cast<FEXCore::Profiler::ThreadStatsHeader*>(g_stats.shm_base);
  }
}

static uint64_t ConvertToBytes(std::string_view Size, std::string_view Granule) {
  uint64_t SizeBytes {};
  SizeBytes = strtoull(Size.data(), nullptr, 10);

  // Granule should only be in kB.
  if (Granule == "kB") {
    SizeBytes *= 1024U;
  }
  else {
    exit_screen("Unknown size modifier: %s\n", std::string(Granule).c_str());
  }

  return SizeBytes;
}

static std::string ConvertMemToHuman(uint64_t MemBytes) {
  const char *Granule;
  if (MemBytes >= (1024 * 1024)) {
    MemBytes /= 1024 * 1024;
    Granule = "MiB";
  } else if (MemBytes >= 1024) {
    MemBytes /= 1024;
    Granule = "KiB";
  }
  return std::format("{} {}", MemBytes, Granule);
}

static void ResidentFEXAnonSampling() {
  const auto fex_pid_smaps = std::format("/proc/{}/smaps", g_stats.pid);

  const int smap_fd = open(fex_pid_smaps.c_str(), O_RDONLY);

  if (smap_fd == -1) return;

  std::string File{};
  while (!g_stats.ShuttingDown.load()) {

    // Read the full file again.
    File.clear();
    lseek(smap_fd, 0, SEEK_SET);
    char TempBuffer[4096];
    ssize_t ReadSize {};
    while ((ReadSize = read(smap_fd, TempBuffer, 4096)) > 0) {
      File.append(TempBuffer, ReadSize);
    }

    if (ReadSize == -1) {
      // Error.
      goto exit;
    }

    // Parse the file by line.
    uint64_t TotalResident {};
    uint64_t TotalJITResident {};
    uint64_t TotalOpDispatcherResident {};
    uint64_t TotalFrontendResident {};
    uint64_t TotalCPUBackendResident {};
    uint64_t TotalLookupResident {};
    uint64_t TotalLookupL1Resident {};
    uint64_t TotalThreadStateResident {};
    uint64_t TotalBlockLinksResident {};
    uint64_t TotalMiscResident{};
    uint64_t TotalJEMallocResident{};
    uint64_t TotalUnaccounted{};
    fex_stats::FEXMemStats::LargestAnonType LargestRSSAnon {};

    uint64_t Begin {}, End {};
    uint64_t *ActiveSubRegion {};

    std::istringstream ss(File);
    std::string Line;
    while (std::getline(ss, Line)) {
      // `359519000-359918000 ---p 00000000 00:00 0                                [anon:FEXMem]`
      if (Line.find("FEXMem") != Line.npos) {
        sscanf(Line.c_str(), "%lx-%lx", &Begin, &End);

        if (Line.find("FEXMemJIT") != Line.npos) {
          ActiveSubRegion = &TotalJITResident;
        } else if (Line.find("FEXMem_OpDispatcher") != Line.npos) {
          ActiveSubRegion = &TotalOpDispatcherResident;
        } else if (Line.find("FEXMem_Frontend") != Line.npos) {
          ActiveSubRegion = &TotalFrontendResident;
        } else if (Line.find("FEXMem_CPUBackend") != Line.npos) {
          ActiveSubRegion = &TotalCPUBackendResident;
        } else if (Line.find("FEXMem_Lookup_L1") != Line.npos) {
          ActiveSubRegion = &TotalLookupL1Resident;
        } else if (Line.find("FEXMem_Lookup") != Line.npos) {
          ActiveSubRegion = &TotalLookupResident;
        } else if (Line.find("FEXMem_ThreadState") != Line.npos) {
          ActiveSubRegion = &TotalThreadStateResident;
        } else if (Line.find("FEXMem_BlockLinks") != Line.npos) {
          ActiveSubRegion = &TotalBlockLinksResident;
        } else if (Line.find("FEXMem_Misc") != Line.npos) {
          ActiveSubRegion = &TotalMiscResident;
        } else {
          // Fully anonymous.
          ActiveSubRegion = &TotalUnaccounted;
        }

        continue;
      }

      if (Line.find("JEMalloc") != Line.npos) {
        ActiveSubRegion = &TotalJEMallocResident;
        continue;
      }

      if (Line.find("VmFlags") != Line.npos) {
        ActiveSubRegion = nullptr;
        continue;
      }

      if (ActiveSubRegion && Line.find("Rss") != Line.npos) {
        // Parse the residency for this mapped region and add it.
        // ex: `Rss:                 560 kB`
        auto GranuleIter = Line.find_last_of(' ') + 1;
        auto SizeIter = Line.find_last_of(' ', GranuleIter - 2) + 1;

        std::string_view GranuleView = std::string_view(&Line.at(GranuleIter));
        std::string_view SizeView = std::string_view(&Line.at(SizeIter), GranuleIter - SizeIter - 1);
        uint64_t ResidentInBytes = ConvertToBytes(SizeView, GranuleView);
        TotalResident += ResidentInBytes;
        *ActiveSubRegion += ResidentInBytes;

        if (ActiveSubRegion == &TotalUnaccounted) {
          if (LargestRSSAnon.Size < ResidentInBytes) {
            LargestRSSAnon = {
              .Begin = Begin,
              .End = End,
              .Size = ResidentInBytes,
            };
          }
        }
        continue;
      }
    }

    if (TotalResident) {
      g_stats.MemStats.LargestAnon = LargestRSSAnon;

      g_stats.MemStats.TotalAnon.store(TotalResident);
      g_stats.MemStats.JITCode.store(TotalJITResident);
      g_stats.MemStats.OpDispatcher.store(TotalOpDispatcherResident);
      g_stats.MemStats.Frontend.store(TotalFrontendResident);
      g_stats.MemStats.CPUBackend.store(TotalCPUBackendResident);
      g_stats.MemStats.Lookup.store(TotalLookupResident);
      g_stats.MemStats.LookupL1.store(TotalLookupL1Resident);
      g_stats.MemStats.ThreadStates.store(TotalThreadStateResident);
      g_stats.MemStats.BlockLinks.store(TotalBlockLinksResident);
      g_stats.MemStats.Misc.store(TotalMiscResident);
      g_stats.MemStats.JEMalloc.store(TotalJEMallocResident);
      g_stats.MemStats.Unaccounted.store(TotalUnaccounted);
    }
    std::this_thread::sleep_for(SamplePeriod);
  }

exit:
  close(smap_fd);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: %s [options] <pid>\n", argv[0]);
    return 0;
  }
  g_stats.pid_str = argv[argc - 1];
  g_stats.pid = strtol(g_stats.pid_str, nullptr, 10);

  setup_signal_handler();

  const auto fex_shm = std::format("fex-{}-stats", g_stats.pid_str);
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

  g_stats.pidfd_watch = ::syscall(SYS_pidfd_open, g_stats.pid, 0);
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
  printw("Header for PID %d:\n", g_stats.pid);
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

  std::thread ResidentAnonThread {ResidentFEXAnonSampling};

  const char *ExitString {};

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
          ExitString = "FEX process exited\n";
          goto exit;
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

      constexpr auto TopOfThreads = 20;
      mvprintw(LINES - TopOfThreads - minimum_hot_threads - HistogramHeight, 0, "%ld threads executing\n", minimum_hot_threads);

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

        const auto y_offset = LINES - TopOfThreads - i - HistogramHeight;
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

      mvprintw(LINES - 19 - HistogramHeight, 0, "Total (%zd millisecond sample period):\n",
               std::chrono::duration_cast<std::chrono::milliseconds>(SamplePeriod).count());
      mvprintw(LINES - 18 - HistogramHeight, 0, "       JIT Time: %f %s (%.2f percent)\n", JITSeconds * Scale, ScaleStr,
               JITSeconds / (double)MaxActiveThreads * 100.0);
      mvprintw(LINES - 17 - HistogramHeight, 0, "    Signal Time: %f %s (%.2f percent)\n", SignalTime * Scale, ScaleStr,
               SignalTime / (double)MaxActiveThreads * 100.0);

      double SIGBUS_l = SIGBUSCount;
      double SIGBUS_Per_Second = SIGBUS_l * (SamplePeriodNanoseconds / NanosecondsInSeconds);
      mvprintw(LINES - 16 - HistogramHeight, 0, "     SIGBUS Cnt: %ld (%lf per second)\n", SIGBUSCount, SIGBUS_Per_Second);
      mvprintw(LINES - 15 - HistogramHeight, 0, "        SMC Cnt: %ld\n", SMCCount);
      mvprintw(LINES - 14 - HistogramHeight, 0, "  Softfloat Cnt: %ld\n", FloatFallbackCount);
    }

    g_stats.fex_load_histogram.erase(g_stats.fex_load_histogram.begin());
    g_stats.fex_load_histogram.push_back(fex_load);

    size_t HistogramWidth = COLS - 2;
    for (size_t i = 0; i < HistogramHeight - 1; ++i) {
      mvprintw(LINES - i - 1, 0, "[");
      mvprintw(LINES - i - 1, COLS - 1, "]");
    }

    mvprintw(LINES - 13 - HistogramHeight, 0, "FEX JIT Load: %f (cycles: %ld)\n", fex_load, total_jit_time);

    const uint64_t MemBytes = g_stats.MemStats.TotalAnon.load();
    const uint64_t MemBytesJIT = g_stats.MemStats.JITCode.load();
    const uint64_t MemBytesOpDispatcher = g_stats.MemStats.OpDispatcher.load();
    const uint64_t MemBytesFrontend = g_stats.MemStats.Frontend.load();
    const uint64_t MemBytesCPUBackend = g_stats.MemStats.CPUBackend.load();
    const uint64_t MemBytesLookup = g_stats.MemStats.Lookup.load();
    const uint64_t MemBytesLookupL1 = g_stats.MemStats.LookupL1.load();
    const uint64_t MemBytesThreadStates = g_stats.MemStats.ThreadStates.load();
    const uint64_t MemBytesBlockLinks = g_stats.MemStats.BlockLinks.load();
    const uint64_t MemBytesMisc = g_stats.MemStats.Misc.load();
    const uint64_t MemBytesJEMalloc = g_stats.MemStats.JEMalloc.load();
    const uint64_t MemBytesUnaccounted = g_stats.MemStats.Unaccounted.load();

    constexpr static size_t TotalMemLines = 11;

    if (MemBytes == ~0ULL) {
      mvprintw(LINES - TotalMemLines - HistogramHeight, 0, "Total FEX Anon memory resident: Couldn't detect\n");

    } else {
      std::string SizeHuman = ConvertMemToHuman(MemBytes);
      std::string SizeHumanJIT = ConvertMemToHuman(MemBytesJIT);
      std::string SizeHumanOpDispatcher = ConvertMemToHuman(MemBytesOpDispatcher);
      std::string SizeHumanFrontend = ConvertMemToHuman(MemBytesFrontend);
      std::string SizeHumanCPUBackend = ConvertMemToHuman(MemBytesCPUBackend);
      std::string SizeHumanLookup = ConvertMemToHuman(MemBytesLookup);
      std::string SizeHumanLookupL1 = ConvertMemToHuman(MemBytesLookupL1);
      std::string SizeHumanThreadStates = ConvertMemToHuman(MemBytesThreadStates);
      std::string SizeHumanBlockLinks = ConvertMemToHuman(MemBytesBlockLinks);
      std::string SizeHumanMisc = ConvertMemToHuman(MemBytesMisc);
      std::string SizeHumanJEMalloc = ConvertMemToHuman(MemBytesJEMalloc);
      std::string SizeHumanUnaccounted = ConvertMemToHuman(MemBytesUnaccounted);
      std::string SizeHumanLargestUnaccounted = ConvertMemToHuman(g_stats.MemStats.LargestAnon.Size);

      mvprintw(LINES - TotalMemLines + 0 - HistogramHeight, 0,  "Total FEX Anon memory resident: %s\n", SizeHuman.c_str());
      mvprintw(LINES - TotalMemLines + 1 - HistogramHeight, 0,  "    JIT resident:             %s\n", SizeHumanJIT.c_str());
      mvprintw(LINES - TotalMemLines + 2 - HistogramHeight, 0,  "    OpDispatcher resident:    %s\n", SizeHumanOpDispatcher.c_str());
      mvprintw(LINES - TotalMemLines + 3 - HistogramHeight, 0,  "    Frontend resident:        %s\n", SizeHumanFrontend.c_str());
      mvprintw(LINES - TotalMemLines + 4 - HistogramHeight, 0,  "    CPUBackend resident:      %s\n", SizeHumanCPUBackend.c_str());
      mvprintw(LINES - TotalMemLines + 5 - HistogramHeight, 0,  "    Lookup cache resident:    %s\n", SizeHumanLookup.c_str());
      mvprintw(LINES - TotalMemLines + 6 - HistogramHeight, 0,  "    Lookup L1 cache resident: %s\n", SizeHumanLookupL1.c_str());
      mvprintw(LINES - TotalMemLines + 7 - HistogramHeight, 0,  "    ThreadStates resident:    %s\n", SizeHumanThreadStates.c_str());
      mvprintw(LINES - TotalMemLines + 8 - HistogramHeight, 0,  "    BlockLinks resident:      %s\n", SizeHumanBlockLinks.c_str());
      mvprintw(LINES - TotalMemLines + 9 - HistogramHeight, 0,  "          Misc resident:      %s\n", SizeHumanMisc.c_str());
      mvprintw(LINES - TotalMemLines + 10 - HistogramHeight, 0, "    JEMalloc resident:        %s\n", SizeHumanJEMalloc.c_str());
      mvprintw(LINES - TotalMemLines + 11 - HistogramHeight, 0, "    Unaccounted resident:     %s\n", SizeHumanUnaccounted.c_str());
      mvprintw(LINES - TotalMemLines + 12 - HistogramHeight, 0, "                 Largest:     %s [0x%lx, 0x%lx) - p (void*) memset(0x%lx, 0xFF, %ld)\n",
          SizeHumanLargestUnaccounted.c_str(), g_stats.MemStats.LargestAnon.Begin, g_stats.MemStats.LargestAnon.End, g_stats.MemStats.LargestAnon.Begin, g_stats.MemStats.LargestAnon.End - g_stats.MemStats.LargestAnon.Begin);
    }

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

exit:
  g_stats.ShuttingDown = true;
  close(g_stats.shm_fd);
  close(g_stats.pidfd_watch);
  ResidentAnonThread.join();
  exit_screen(ExitString);
  return 0;
}
