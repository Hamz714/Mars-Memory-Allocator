// Runner for the test framework declared in mars_test.h.

#include "mars_test.h"

#include <stdlib.h>
#include <string.h>

static mars_test_case *g_head;
static uint64_t g_seed = 1;

uint64_t mars_test_seed(void) { return g_seed; }

// Insert sorted by (suite, name). Constructor execution order is unspecified,
// so sorting here is what makes the run order deterministic.
void mars_test_register(mars_test_case *tc) {
  mars_test_case **slot = &g_head;
  while (*slot != NULL) {
    int cmp = strcmp((*slot)->suite, tc->suite);
    if (cmp == 0) cmp = strcmp((*slot)->name, tc->name);
    if (cmp > 0) break;
    slot = &(*slot)->next;
  }
  tc->next = *slot;
  *slot = tc;
}

static void usage(const char *argv0) {
  printf("Usage: %s [options]\n", argv0);
  printf("  --list           list registered tests and exit\n");
  printf("  --filter <str>   run only tests whose suite.name contains <str>\n");
  printf("  --seed <n>       seed available to tests via mars_test_seed()\n");
  printf("  --help           show this help\n");
}

int main(int argc, char **argv) {
  const char *filter = NULL;
  int list_only = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--list") == 0) {
      list_only = 1;
    } else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
      filter = argv[++i];
    } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      g_seed = strtoull(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "unknown argument: %s\n", argv[i]);
      usage(argv[0]);
      return 2;
    }
  }

  char label[256];
  int total = 0, passed = 0, failed = 0, skipped = 0;

  for (mars_test_case *tc = g_head; tc != NULL; tc = tc->next) {
    snprintf(label, sizeof(label), "%s.%s", tc->suite, tc->name);

    if (filter != NULL && strstr(label, filter) == NULL) {
      skipped++;
      continue;
    }
    if (list_only) {
      printf("%s\n", label);
      continue;
    }

    total++;
    int failures = 0;
    tc->fn(&failures);

    if (failures == 0) {
      printf("[  ok  ] %s\n", label);
      passed++;
    } else {
      printf("[ FAIL ] %s (%d)\n", label, failures);
      failed++;
    }
    fflush(stdout);
  }

  if (list_only) return 0;

  printf("\n%d run, %d passed, %d failed", total, passed, failed);
  if (skipped > 0) printf(", %d filtered out", skipped);
  printf("  (seed %" PRIu64 ")\n", g_seed);

  // A filter that matches nothing is an error. Reporting success would make a
  // typo -- or a stray carriage return -- indistinguishable from a clean run.
  if (total == 0) {
    fprintf(stderr, "error: no tests matched%s%s\n", filter ? " filter " : "",
            filter ? filter : "");
    return 2;
  }

  return failed > 0 ? 1 : 0;
}
