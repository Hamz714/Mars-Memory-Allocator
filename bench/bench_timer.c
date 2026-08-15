// clock_gettime and CLOCK_MONOTONIC are POSIX, not ISO C, and the project
// compiles as strict c11 -- so they have to be asked for explicitly, before
// any header is pulled in.
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "bench_timer.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <time.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#  define BENCH_HAVE_TSC 1
#  if defined(_MSC_VER)
#    include <intrin.h>
#  else
#    include <x86intrin.h>
#  endif
#else
#  define BENCH_HAVE_TSC 0
#endif

// --- Wall clock ------------------------------------------------------------

uint64_t bench_wall_ns(void) {
#if defined(_WIN32)
  static LARGE_INTEGER freq;
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  // Split the division to keep the multiply from overflowing on long runs.
  return (uint64_t)(now.QuadPart / freq.QuadPart) * 1000000000ull +
         (uint64_t)((now.QuadPart % freq.QuadPart) * 1000000000ull /
                    (uint64_t)freq.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t bench_ticks(void) {
#if BENCH_HAVE_TSC
  unsigned aux;
  return (uint64_t)__rdtscp(&aux);
#else
  return bench_wall_ns();
#endif
}

// --- Calibration -----------------------------------------------------------

static bench_timer_info g_info;
static bool g_ready;

// Reads the CPU flags that say whether the counter ticks at a fixed rate and
// keeps running through idle states. Without both, TSC deltas are not
// comparable and the harness says so rather than pretending otherwise.
static void probe_tsc_flags(bench_timer_info *info) {
#if defined(__linux__)
  FILE *f = fopen("/proc/cpuinfo", "r");
  if (f == NULL) {
    info->tsc_flags_known = false;
    return;
  }
  char line[4096];
  bool constant = false, nonstop = false, seen_flags = false;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (strncmp(line, "flags", 5) != 0) continue;
    seen_flags = true;
    if (strstr(line, " constant_tsc") != NULL) constant = true;
    if (strstr(line, " nonstop_tsc") != NULL) nonstop = true;
    break;
  }
  fclose(f);
  info->tsc_flags_known = seen_flags;
  info->tsc_usable = constant && nonstop;
#else
  // No portable way to ask. Assume usable but record that it was not checked,
  // so a report can carry the caveat.
  info->tsc_flags_known = false;
  info->tsc_usable = BENCH_HAVE_TSC ? true : false;
#endif
}

static void spin_ns(uint64_t ns) {
  uint64_t deadline = bench_wall_ns() + ns;
  while (bench_wall_ns() < deadline) {
    // busy wait: sleeping would let the core drop into a lower state
  }
}

const bench_timer_info *bench_timer_init(void) {
  if (g_ready) return &g_info;
  memset(&g_info, 0, sizeof(g_info));

  probe_tsc_flags(&g_info);

  // Calibrate against the wall clock. Take the best of a few short intervals:
  // an interval that got descheduled reads slow, never fast.
  double best_ns_per_tick = 0.0;
  for (int attempt = 0; attempt < 5; attempt++) {
    uint64_t w0 = bench_wall_ns();
    uint64_t t0 = bench_ticks();
    spin_ns(20 * 1000 * 1000);  // 20 ms
    uint64_t t1 = bench_ticks();
    uint64_t w1 = bench_wall_ns();

    uint64_t dt = t1 - t0;
    uint64_t dw = w1 - w0;
    if (dt == 0) continue;
    double ratio = (double)dw / (double)dt;
    if (best_ns_per_tick == 0.0 || ratio < best_ns_per_tick) {
      best_ns_per_tick = ratio;
    }
  }
  g_info.ns_per_tick = best_ns_per_tick > 0.0 ? best_ns_per_tick : 1.0;

  // Cost of the measurement itself: the minimum over many back-to-back pairs,
  // since anything above the minimum is interference rather than the timer.
  uint64_t best = UINT64_MAX;
  for (int i = 0; i < 20000; i++) {
    uint64_t a = bench_ticks();
    uint64_t b = bench_ticks();
    uint64_t d = b - a;
    if (d < best) best = d;
  }
  g_info.overhead_ticks = (best == UINT64_MAX) ? 0 : best;
  g_info.overhead_ns = (double)g_info.overhead_ticks * g_info.ns_per_tick;

  g_ready = true;
  return &g_info;
}

double bench_ticks_to_ns(uint64_t ticks) {
  if (!g_ready) bench_timer_init();
  double ns = (double)ticks * g_info.ns_per_tick - g_info.overhead_ns;
  return ns < 0.0 ? 0.0 : ns;
}

// --- Histogram -------------------------------------------------------------

void bench_hist_reset(bench_hist *h) {
  memset(h, 0, sizeof(*h));
  h->min = UINT64_MAX;
}

static size_t hist_index(uint64_t value) {
  if (value < BENCH_HIST_SUB_COUNT) return (size_t)value;

  // Highest set bit decides the octave; the next SUB_BITS bits index within it.
  int msb = 63;
  while (((value >> msb) & 1u) == 0) msb--;

  int shift = msb - BENCH_HIST_SUB_BITS + 1;
  size_t bucket = (size_t)shift;
  size_t sub = (size_t)((value >> shift) & (BENCH_HIST_SUB_COUNT - 1));
  size_t index = (bucket + 1) * BENCH_HIST_SUB_COUNT + sub;
  return index >= BENCH_HIST_SLOTS ? BENCH_HIST_SLOTS - 1 : index;
}

// Lowest value that lands in a slot, used when reporting a quantile. The
// sub-bucket index already carries the value's leading one bit -- hist_index
// masks the shifted value rather than stripping that bit -- so reconstructing
// it must not add the bit back.
static uint64_t hist_value_at(size_t index) {
  if (index < BENCH_HIST_SUB_COUNT) return (uint64_t)index;
  int shift = (int)(index / BENCH_HIST_SUB_COUNT) - 1;
  uint64_t sub = (uint64_t)(index % BENCH_HIST_SUB_COUNT);
  return sub << shift;
}

void bench_hist_record(bench_hist *h, uint64_t value) {
  h->slots[hist_index(value)]++;
  h->count++;
  h->total += value;
  if (value < h->min) h->min = value;
  if (value > h->max) h->max = value;
}

uint64_t bench_hist_quantile(const bench_hist *h, double q) {
  if (h->count == 0) return 0;
  if (q <= 0.0) return h->min;
  if (q >= 1.0) return h->max;

  uint64_t want = (uint64_t)(q * (double)h->count);
  if (want == 0) want = 1;

  uint64_t seen = 0;
  for (size_t i = 0; i < BENCH_HIST_SLOTS; i++) {
    seen += h->slots[i];
    if (seen >= want) {
      uint64_t v = hist_value_at(i);
      return v > h->max ? h->max : v;
    }
  }
  return h->max;
}

double bench_hist_mean(const bench_hist *h) {
  return h->count == 0 ? 0.0 : (double)h->total / (double)h->count;
}
