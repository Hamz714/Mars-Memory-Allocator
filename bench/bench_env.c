// The `#` provenance block every CSV in bench/results/ opens with.
//
// Shared by both drivers rather than written twice, because the whole point of
// it is that two files can be compared: a field one driver records and the
// other does not is a difference nobody can see when reading the results.

#define _POSIX_C_SOURCE 200809L

#include "bench.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(__linux__)
#  include <sys/utsname.h>
#endif

#include "mars/allocator.h"
#include "mm_lock.h"

void bench_write_env(FILE *out, const char *git_sha) {
  char cpu[256] = "unknown";
  char kernel[256] = "unknown";

#if defined(__linux__)
  FILE *f = fopen("/proc/cpuinfo", "r");
  if (f != NULL) {
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
      if (strncmp(line, "model name", 10) == 0) {
        char *colon = strchr(line, ':');
        if (colon != NULL) {
          char *v = colon + 1;
          while (*v == ' ') v++;
          size_t len = strlen(v);
          while (len > 0 && (v[len - 1] == '\n' || v[len - 1] == ' ')) len--;
          snprintf(cpu, sizeof(cpu), "%.*s", (int)len, v);
        }
        break;
      }
    }
    fclose(f);
  }
  struct utsname u;
  if (uname(&u) == 0) snprintf(kernel, sizeof(kernel), "%s %s", u.sysname, u.release);
#elif defined(_WIN32)
  snprintf(kernel, sizeof(kernel), "windows");
#endif

  const bench_timer_info *t = bench_timer_init();
  time_t now = time(NULL);
  char when[64];
  strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

  fprintf(out, "# cpu=%s\n", cpu);
  fprintf(out, "# kernel=%s\n", kernel);
  fprintf(out, "# compiler=%s\n",
#if defined(__clang__)
          "clang " __clang_version__
#elif defined(__GNUC__)
          "gcc " __VERSION__
#else
          "unknown"
#endif
  );
  fprintf(out, "# date=%s\n", when);
  fprintf(out, "# git_sha=%s\n", git_sha);
  fprintf(out, "# tsc_usable=%d tsc_flags_known=%d ns_per_tick=%.6f\n",
          t->tsc_usable ? 1 : 0, t->tsc_flags_known ? 1 : 0, t->ns_per_tick);
  fprintf(out, "# timer_overhead_ns=%.2f\n", t->overhead_ns);
  // The profile decides how much metadata a block carries and how much
  // validation each access does, so two runs are only comparable when it
  // matches. Recording it here is what makes that checkable after the fact.
  fprintf(out, "# profile=%s metadata_bytes=%zu\n", mm_profile(),
          mm_metadata_overhead());
#ifdef MM_STATS
  fprintf(out, "# counters=on\n");
#else
  fprintf(out, "# counters=off\n");
#endif
}
