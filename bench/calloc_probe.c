// A probe for one specific question: what does knowing a mapping is fresh save
// calloc?
//
// A calloc of a large buffer is served by a mapping the kernel has just
// supplied, and those pages are already zero. An allocator that knows this
// hands the buffer back untouched; one that does not memsets every byte of it,
// faulting in the whole allocation to write zeroes over zeroes. glibc has made
// that distinction for decades. This program is what prices it here.
//
// It links against nothing -- no mars library, no flags of ours -- because it
// has to be able to run under LD_PRELOAD against whichever allocator is being
// measured, including none of ours at all.
//
//   calloc_probe <count> <bytes> [touch]
//
// `touch` reads one byte per page rather than one per buffer, which is the
// difference between measuring the allocator and measuring the page faults the
// program would have taken anyway. Both are interesting and they answer
// different questions, so it is a parameter rather than a decision.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <count> <bytes> [touch]\n", argv[0]);
    return 2;
  }
  unsigned long count = strtoul(argv[1], NULL, 10);
  unsigned long bytes = strtoul(argv[2], NULL, 10);
  int touch_pages = argc > 3 && argv[3][0] == 't';
  if (count == 0 || bytes == 0) return 2;

  // Accumulated and printed, so that nothing here can be optimised away and so
  // that a buffer which is not actually zero is visible as a wrong answer
  // rather than as a fast one.
  unsigned long long sum = 0;

  for (unsigned long i = 0; i < count; i++) {
    unsigned char *p = (unsigned char *)calloc(bytes, 1);
    if (p == NULL) {
      fprintf(stderr, "calloc(%lu) failed at %lu\n", bytes, i);
      return 1;
    }
    if (touch_pages) {
      for (unsigned long off = 0; off < bytes; off += 4096) sum += p[off];
    } else {
      sum += p[0] + p[bytes / 2] + p[bytes - 1];
    }
    free(p);
  }

  printf("%llu\n", sum);
  return sum == 0 ? 0 : 3;  // anything non-zero means calloc did not zero
}
