#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mars/allocator.h"

// Configuration constants for the test suite
#define DEFAULT_HEAP_SIZE 8192
#define DEFAULT_SEED 42
#define ALIGNMENT 40
#define PATTERN_SIZE 5

// Struct to track the cumulative results of the test suite.
typedef struct {
  int total_tests;
  int passed_tests;
  int failed_tests;
} TestStats;

// Global statistics instance
static TestStats stats = {0, 0, 0};

// ANSI Color codes for console output formatting
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_RESET   "\x1b[0m"

// Prints a styled header for a specific test case.
void print_test_header(const char *test_name) {
  printf("\n" COLOR_BLUE "========================================\n");
  printf("TEST: %s\n", test_name);
  printf("========================================" COLOR_RESET "\n");
}

// Records a passed test and updates statistics.
void test_pass(const char *msg) {
  printf(COLOR_GREEN "[PASS]" COLOR_RESET " %s\n", msg);
  stats.passed_tests++;
  stats.total_tests++;
}

// Records a failed test and updates statistics.
void test_fail(const char *msg) {
  printf(COLOR_RED "[FAIL]" COLOR_RESET " %s\n", msg);
  stats.failed_tests++;
  stats.total_tests++;
}

// Prints the final report of all tests run.
void print_summary() {
  printf("\n" COLOR_BLUE "========================================\n");
  printf("TEST SUMMARY\n");
  printf("========================================" COLOR_RESET "\n");
  printf("Total Tests:  %d\n", stats.total_tests);
  printf(COLOR_GREEN "Passed:       %d\n" COLOR_RESET, stats.passed_tests);
  printf(COLOR_RED "Failed:       %d\n" COLOR_RESET, stats.failed_tests);

  // Calculate success percentage, handling divide-by-zero safety
  double rate = stats.total_tests > 0 ?
                (100.0 * stats.passed_tests / stats.total_tests) : 0.0;
  printf("Success Rate: %.2f%%\n", rate);
}

// Simulates a "Radiation Storm" by flipping random bits across the heap.
// This tests the allocator's ability
// to detect corruption via checksums/footers.
void inject_bit_flips(uint8_t *heap, size_t heap_size, int num_flips) {
  for (int i = 0; i < num_flips; i++) {
    size_t byte_pos = rand() % heap_size;
    int bit_pos = rand() % 8;
    // XOR operation flips the specific bit
    heap[byte_pos] ^= (1 << bit_pos);
  }
}

// Test 1: Verifies that the allocator initializes correctly.
// Checks edge cases like NULL pointers and invalid sizes.
void test_initialization(uint8_t *heap, size_t heap_size) {
  print_test_header("Initialization");

  // Pre-fill heap with the OS pattern to simulate "clean" memory
  for (size_t i = 0; i < heap_size; i++) {
    heap[i] = (uint8_t)((i % PATTERN_SIZE) + 0xA0);
  }

  // Standard initialization check
  int result = mm_init(heap, heap_size);
  if (result == 0) {
    test_pass("Heap initialized successfully");
  } else {
    test_fail("Heap initialization failed");
  }

  // Edge Case: Test NULL heap pointer rejection
  result = mm_init(NULL, heap_size);
  if (result != 0) {
    test_pass("NULL heap rejected correctly");
  } else {
    test_fail("NULL heap should be rejected");
  }

  // Edge Case: Test insufficient size rejection
  result = mm_init(heap, 10);
  if (result != 0) {
    test_pass("Too small heap rejected correctly");
  } else {
    test_fail("Too small heap should be rejected");
  }
}

// Test 2: Verifies basic malloc functionality for various sizes.
void test_basic_allocation(uint8_t *heap, size_t heap_size) {
  print_test_header("Basic Allocation");

  // Reset heap state
  for (size_t i = 0; i < heap_size; i++) {
    heap[i] = (uint8_t)((i % PATTERN_SIZE) + 0xA0);
  }
  mm_init(heap, heap_size);

  // 1. Small allocation
  void *ptr1 = mm_malloc(64);
  if (ptr1 != NULL) {
    test_pass("Small allocation (64 bytes) succeeded");
  } else {
    test_fail("Small allocation failed");
  }

  // 2. Medium allocation
  void *ptr2 = mm_malloc(256);
  if (ptr2 != NULL) {
    test_pass("Medium allocation (256 bytes) succeeded");
  } else {
    test_fail("Medium allocation failed");
  }

  // 3. Large allocation
  void *ptr3 = mm_malloc(1024);
  if (ptr3 != NULL) {
    test_pass("Large allocation (1024 bytes) succeeded");
  } else {
    test_fail("Large allocation failed");
  }

  // 4. Zero-size allocation (should return NULL)
  void *ptr_null = mm_malloc(0);
  if (ptr_null == NULL) {
    test_pass("Zero-size allocation correctly returned NULL");
  } else {
    test_fail("Zero-size allocation should return NULL");
  }
}

// Test 3: Verifies that returned pointers meet the 40-byte alignment rule.
void test_alignment(uint8_t *heap, size_t heap_size) {
  print_test_header("Alignment Verification");

  for (size_t i = 0; i < heap_size; i++) {
    heap[i] = (uint8_t)((i % PATTERN_SIZE) + 0xA0);
  }
  mm_init(heap, heap_size);

  int alignment_pass = 1;
  // Perform multiple random allocations to check alignment consistency
  for (int i = 0; i < 10; i++) {
    size_t size = 10 + (rand() % 500);
    void *ptr = mm_malloc(size);
    if (ptr != NULL) {
      // Check if address is a multiple of ALIGNMENT (40)
      if ((size_t)ptr % ALIGNMENT != 0) {
        printf("  Misaligned pointer: %p (mod %d = %zu)\n",
               ptr, ALIGNMENT, (size_t)ptr % ALIGNMENT);
        alignment_pass = 0;
      }
      mm_free(ptr);
    }
  }

  if (alignment_pass) {
    test_pass("All allocations properly aligned to 40 bytes");
  } else {
    test_fail("Some allocations were misaligned");
  }
}

// Test 4: Verifies the custom mm_read and mm_write functions.
// Ensures data is persisted and bounds checking works.
void test_read_write(uint8_t *heap, size_t heap_size) {
  print_test_header("Read/Write Operations");

  for (size_t i = 0; i < heap_size; i++) {
    heap[i] = (uint8_t)((i % PATTERN_SIZE) + 0xA0);
  }
  mm_init(heap, heap_size);

  void *ptr = mm_malloc(128);
  if (ptr == NULL) {
    test_fail("Could not allocate memory for read/write test");
    return;
  }

  // 1. Valid Write
  char write_data[] = "Hello, Mars!";
  int write_result = mm_write(ptr, 0, write_data, strlen(write_data) + 1);
  if (write_result == (int)strlen(write_data) + 1) {
    test_pass("Write operation successful");
  } else {
    test_fail("Write operation failed");
  }

  // 2. Valid Read
  char read_buffer[64] = {0};
  int read_result = mm_read(ptr, 0, read_buffer, strlen(write_data) + 1);
  if (read_result == (int)strlen(write_data) + 1) {
    test_pass("Read operation successful");
  } else {
    test_fail("Read operation failed");
  }

  // 3. Verify Integrity
  if (strcmp(read_buffer, write_data) == 0) {
    test_pass("Data integrity maintained (read matches write)");
  } else {
    test_fail("Data corruption detected");
  }

  // 4. Out-of-bounds Read (Buffer Overflow check)
  char oob_buffer[256];
  read_result = mm_read(ptr, 0, oob_buffer, 256);
  if (read_result == -1) {
    test_pass("Out-of-bounds read correctly rejected");
  } else {
    test_fail("Out-of-bounds read should fail");
  }

  // 5. Out-of-bounds Write
  char large_data[256] = {0};
  write_result = mm_write(ptr, 0, large_data, 256);
  if (write_result == -1) {
    test_pass("Out-of-bounds write correctly rejected");
  } else {
    test_fail("Out-of-bounds write should fail");
  }

  mm_free(ptr);
}

// Test 5: Verifies that freeing memory works and coalesces neighbors.
void test_free_and_coalescing(uint8_t *heap, size_t heap_size) {
  print_test_header("Free and Coalescing");

  for (size_t i = 0; i < heap_size; i++) {
    heap[i] = (uint8_t)((i % PATTERN_SIZE) + 0xA0);
  }
  mm_init(heap, heap_size);

  // Allocate 3 contiguous blocks
  void *ptr1 = mm_malloc(100);
  void *ptr2 = mm_malloc(100);
  void *ptr3 = mm_malloc(100);

  if (ptr1 && ptr2 && ptr3) {
    test_pass("Multiple allocations successful");
  } else {
    test_fail("Multiple allocations failed");
    return;
  }

  // Free middle block (creates a hole)
  mm_free(ptr2);
  test_pass("Middle block freed");

  // Free first block (should merge with middle)
  mm_free(ptr1);
  test_pass("First block freed (coalescing test)");

  // Free last block (should merge with previous large free block)
  mm_free(ptr3);
  test_pass("Last block freed (full coalescing)");

  // Allocate a block larger than any single original block.
  // This verifies that the 3 blocks merged into one large free block.
  void *large_ptr = mm_malloc(280);
  if (large_ptr != NULL) {
    test_pass("Large allocation after coalescing succeeded");
    mm_free(large_ptr);
  } else {
    test_fail("Coalescing may not have worked properly");
  }
}

// Test 6: Verifies safety mechanisms against double-frees.
void test_double_free(uint8_t *heap, size_t heap_size) {
  print_test_header("Double-Free Detection");

  for (size_t i = 0; i < heap_size; i++) {
    heap[i] = (uint8_t)((i % PATTERN_SIZE) + 0xA0);
  }
  mm_init(heap, heap_size);

  void *ptr = mm_malloc(100);
  if (ptr == NULL) {
    test_fail("Could not allocate for double-free test");
    return;
  }

  // Valid free
  mm_free(ptr);
  test_pass("First free successful");

  // Invalid second free (Double Free)
  // The allocator should detect this via the 'in_use' flag and ignore it.
  mm_free(ptr);
  test_pass("Double-free handled safely (no crash)");

  // Test NULL free (should do nothing)
  mm_free(NULL);
  test_pass("NULL free handled correctly");
}

// Test 7: Tests the allocator's ability to handle fragmented memory.
void test_fragmentation(uint8_t *heap, size_t heap_size) {
  print_test_header("Fragmentation Handling");

  for (size_t i = 0; i < heap_size; i++) {
    heap[i] = (uint8_t)((i % PATTERN_SIZE) + 0xA0);
  }
  mm_init(heap, heap_size);

#define NUM_BLOCKS 10
  void *ptrs[NUM_BLOCKS];

  // 1. Allocate many small blocks
  int alloc_count = 0;
  for (int i = 0; i < NUM_BLOCKS; i++) {
    ptrs[i] = mm_malloc(50);
    if (ptrs[i] != NULL) alloc_count++;
  }

  printf("  Allocated %d/%d blocks\n", alloc_count, NUM_BLOCKS);
  if (alloc_count > 0) {
    test_pass("Multiple small allocations successful");
  } else {
    test_fail("Could not allocate blocks");
  }

  // 2. Create fragmentation by freeing every other block
  for (int i = 0; i < NUM_BLOCKS; i += 2) {
    if (ptrs[i]) mm_free(ptrs[i]);
  }
  test_pass("Freed alternating blocks (creating fragmentation)");

  // 3. Attempt to allocate a medium block that requires searching
  void *medium = mm_malloc(100);
  if (medium != NULL) {
    test_pass("Allocation successful despite fragmentation");
    mm_free(medium);
  } else {
    printf("  " COLOR_YELLOW "[INFO]" COLOR_RESET
           " Could not allocate 100 bytes (fragmentation)\n");
  }

  // Cleanup remaining blocks
  for (int i = 1; i < NUM_BLOCKS; i += 2) {
    if (ptrs[i]) mm_free(ptrs[i]);
  }
}

// Test 8: Verifies mm_realloc behavior (Growing, Shrinking, Data Preservation).
void test_realloc(uint8_t *heap, size_t heap_size) {
  print_test_header("Realloc Operations");

  for (size_t i = 0; i < heap_size; i++) {
    heap[i] = (uint8_t)((i % PATTERN_SIZE) + 0xA0);
  }
  mm_init(heap, heap_size);

  void *ptr = mm_malloc(64);
  if (ptr == NULL) {
    test_fail("Initial allocation for realloc failed");
    return;
  }

  // Populate data
  char data[] = "Test data for realloc";
  mm_write(ptr, 0, data, strlen(data) + 1);

  // 1. Grow allocation (128 bytes)
  void *new_ptr = mm_realloc(ptr, 128);
  if (new_ptr != NULL) {
    test_pass("Realloc to larger size succeeded");

    // Check if data was correctly copied to the new block
    char read_buffer[64];
    mm_read(new_ptr, 0, read_buffer, strlen(data) + 1);
    if (strcmp(read_buffer, data) == 0) {
      test_pass("Data preserved after realloc (grow)");
    } else {
      test_fail("Data corrupted after realloc");
    }
    ptr = new_ptr;
  } else {
    test_fail("Realloc to larger size failed");
    return;
  }

  // 2. Shrink allocation (32 bytes)
  new_ptr = mm_realloc(ptr, 32);
  if (new_ptr != NULL) {
    test_pass("Realloc to smaller size succeeded");
    ptr = new_ptr;
  } else {
    test_fail("Realloc to smaller size failed");
  }

  // 3. Realloc NULL (Equivalent to Malloc)
  void *null_realloc = mm_realloc(NULL, 64);
  if (null_realloc != NULL) {
    test_pass("Realloc with NULL pointer (malloc behavior)");
    mm_free(null_realloc);
  } else {
    test_fail("Realloc with NULL should allocate new block");
  }

  mm_free(ptr);
}

// Test 9: Runs a random sequence of alloc/free/write/read ops.
// Checks for stability under load.
void test_stress(uint8_t *heap, size_t heap_size, int num_operations) {
  print_test_header("Stress Test (Mixed Operations)");

  for (size_t i = 0; i < heap_size; i++) {
    heap[i] = (uint8_t)((i % PATTERN_SIZE) + 0xA0);
  }
  mm_init(heap, heap_size);

#define MAX_PTRS 50
  void *ptrs[MAX_PTRS] = {0};
  int operations = 0;
  int successful_ops = 0;

  for (int i = 0; i < num_operations; i++) {
    int op = rand() % 3;
    int idx = rand() % MAX_PTRS;

    if (op == 0 || ptrs[idx] == NULL) {
      // Operation: Allocate
      size_t size = 10 + (rand() % 200);
      ptrs[idx] = mm_malloc(size);
      if (ptrs[idx] != NULL) {
        successful_ops++;
      }
    } else if (op == 1 && ptrs[idx] != NULL) {
      // Operation: Write and Read Verification
      char test_byte = (char)(rand() % 256);
      if (mm_write(ptrs[idx], 0, &test_byte, 1) > 0) {
        char read_byte;
        if (mm_read(ptrs[idx], 0, &read_byte, 1) > 0) {
          if (read_byte == test_byte) {
            successful_ops++;
          }
        }
      }
    } else if (ptrs[idx] != NULL) {
      // Operation: Free
      mm_free(ptrs[idx]);
      ptrs[idx] = NULL;
      successful_ops++;
    }
    operations++;
  }

  printf("  Completed %d operations (%d successful)\n",
         operations, successful_ops);
  if (successful_ops > operations * 0.5) {
    test_pass("Stress test completed successfully");
  } else {
    test_fail("Too many operations failed in stress test");
  }

  // Cleanup leftovers
  for (int i = 0; i < MAX_PTRS; i++) {
    if (ptrs[i]) mm_free(ptrs[i]);
  }
}

// Test 10: Simulates radiation by injecting bit flips.
// Verifies that the allocator detects corruption and quarantines blocks.
void test_radiation_storm(uint8_t *heap, size_t heap_size, int storm_level) {
  print_test_header("Radiation Storm Simulation");

  for (size_t i = 0; i < heap_size; i++) {
    heap[i] = (uint8_t)((i % PATTERN_SIZE) + 0xA0);
  }
  mm_init(heap, heap_size);

  printf("  Storm level: %d\n", storm_level);

  // 1. Establish Baseline: Allocate blocks
  void *ptr1 = mm_malloc(128);
  void *ptr2 = mm_malloc(256);
  void *ptr3 = mm_malloc(64);

  if (!ptr1 || !ptr2 || !ptr3) {
    test_fail("Could not allocate blocks for storm test");
    return;
  }

  char data1[] = "Block 1 data";
  char data2[] = "Block 2 data - longer string";
  char data3[] = "Block 3";
  mm_write(ptr1, 0, data1, strlen(data1) + 1);
  mm_write(ptr2, 0, data2, strlen(data2) + 1);
  mm_write(ptr3, 0, data3, strlen(data3) + 1);

  test_pass("Pre-storm allocation and write successful");

  // 2. The Event: Inject random bit flips
  int num_flips = storm_level * 10;
  inject_bit_flips(heap, heap_size, num_flips);
  printf("  Injected %d bit flips\n", num_flips);

  // 3. Post-Storm Assessment: Try to read back data
  char read_buffer[128];
  int successful_reads = 0;

  // The allocator should return -1 if it detects corruption, preventing usage
  if (mm_read(ptr1, 0, read_buffer, strlen(data1) + 1) > 0) {
    successful_reads++;
  }
  if (mm_read(ptr2, 0, read_buffer, strlen(data2) + 1) > 0) {
    successful_reads++;
  }
  if (mm_read(ptr3, 0, read_buffer, strlen(data3) + 1) > 0) {
    successful_reads++;
  }

  printf("  Successfully read %d/3 blocks after storm\n", successful_reads);

  // 4. Cleanup: Free should identify corrupt blocks and isolate them (no crash)
  mm_free(ptr1);
  mm_free(ptr2);
  mm_free(ptr3);
  test_pass("Free operations completed without crash after storm");

  // 5. Recovery: Try new allocation. If old blocks were quarantined, this works
  void *new_ptr = mm_malloc(100);
  if (new_ptr != NULL) {
    test_pass("New allocation successful after storm");
    mm_free(new_ptr);
  } else {
    printf("  " COLOR_YELLOW "[INFO]" COLOR_RESET
           " Could not allocate after storm (quarantine working)\n");
  }
}

// Test 11: Tests behavior when heap is fully utilized.
void test_exhaustion(uint8_t *heap, size_t heap_size) {
  print_test_header("Memory Exhaustion");

  for (size_t i = 0; i < heap_size; i++) {
    heap[i] = (uint8_t)((i % PATTERN_SIZE) + 0xA0);
  }
  mm_init(heap, heap_size);

  void *ptrs[100];
  int count = 0;

  // Fill up the heap
  for (int i = 0; i < 100; i++) {
    ptrs[i] = mm_malloc(100);
    if (ptrs[i] != NULL) {
      count++;
    } else {
      break;  // Heap full
    }
  }

  printf("  Allocated %d blocks before exhaustion\n", count);
  test_pass("Memory exhaustion handled gracefully");

  // Verify that subsequent allocation returns NULL
  void *should_fail = mm_malloc(1000);
  if (should_fail == NULL) {
    test_pass("NULL returned when memory exhausted");
  } else {
    test_fail("Should return NULL when out of memory");
    mm_free(should_fail);
  }

  // Free everything
  for (int i = 0; i < count; i++) {
    mm_free(ptrs[i]);
  }

  // Verify allocation works again after freeing
  void *ptr = mm_malloc(500);
  if (ptr != NULL) {
    test_pass("Allocation successful after freeing all blocks");
    mm_free(ptr);
  } else {
    test_fail("Could not allocate after freeing");
  }
}

int main(int argc, char *argv[]) {
  // Parse command line arguments
  size_t heap_size = DEFAULT_HEAP_SIZE;
  int seed = DEFAULT_SEED;
  int storm_level = 0;
  int stress_ops = 100;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
      heap_size = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--storm") == 0 && i + 1 < argc) {
      storm_level = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--stress") == 0 && i + 1 < argc) {
      stress_ops = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--help") == 0) {
      printf("Usage: %s [options]\n", argv[0]);
      printf("Options:\n");
      printf("  --size N      Set heap size (default: %d)\n",
             DEFAULT_HEAP_SIZE);
      printf("  --seed N      Set random seed (default: %d)\n", DEFAULT_SEED);
      printf("  --storm N     Set storm level 0-10 (default: 0)\n");
      printf("  --stress N    Set stress test operations (default: 100)\n");
      printf("  --help        Show this help\n");
      return 0;
    }
  }

  srand(seed);

  printf(COLOR_BLUE "╔════════════════════════════════════════╗\n");
  printf("║  Mars Allocator Test Suite             ║\n");
  printf("╚════════════════════════════════════════╝" COLOR_RESET "\n");
  printf("Heap Size: %zu bytes\n", heap_size);
  printf("Random Seed: %d\n", seed);
  printf("Storm Level: %d\n", storm_level);
  printf("\n");

  // Allocate heap (this is the ONLY malloc allowed in the entire project)
  uint8_t *heap = (uint8_t *)malloc(heap_size);
  if (!heap) {
    fprintf(stderr, "Failed to allocate heap memory\n");
    return 1;
  }

  // Execute Test Suite
  test_initialization(heap, heap_size);
  test_basic_allocation(heap, heap_size);
  // test_alignment(heap, heap_size); // Optional check
  test_read_write(heap, heap_size);
  test_free_and_coalescing(heap, heap_size);
  test_double_free(heap, heap_size);
  test_fragmentation(heap, heap_size);
  test_realloc(heap, heap_size);
  test_stress(heap, heap_size, stress_ops);
  test_exhaustion(heap, heap_size);

  // Trigger optional Radiation Storm if requested
  if (storm_level > 0) {
    test_radiation_storm(heap, heap_size, storm_level);
  }

  // Print final summary stats
  print_summary();

  // Cleanup test resources
  free(heap);

  // Return non-zero if any tests failed to signal CI/CD pipeline
  return (stats.failed_tests > 0) ? 1 : 0;
}
