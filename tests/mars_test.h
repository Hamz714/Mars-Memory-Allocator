// Minimal assertion-based test framework.
//
// Tests self-register at load time, so declaring one is all that is needed:
//
//   MM_TEST(core, malloc_returns_aligned_pointer) {
//     void *p = mm_malloc(64);
//     REQUIRE_NOT_NULL(p);
//     CHECK_EQ((uintptr_t)p % mm_alignment(), 0);
//   }
//
// CHECK_* records a failure and continues; REQUIRE_* records and returns, for
// cases where carrying on would crash. Link a binary against mars_test to get
// the runner's main().

#ifndef MARS_TEST_H_
#define MARS_TEST_H_

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct mars_test_case mars_test_case;

// Each test body receives a pointer to its own failure counter.
typedef void (*mars_test_fn)(int *mm_failures_);

struct mars_test_case {
  const char *suite;
  const char *name;
  mars_test_fn fn;
  mars_test_case *next;
};

// Registers a test. Called automatically by MM_TEST via a load-time
// constructor; not intended to be called directly.
void mars_test_register(mars_test_case *tc);

// Seed for the current run, set by --seed. Tests that randomise must draw from
// this so a failure can be replayed.
uint64_t mars_test_seed(void);

#define MM_TEST(suite_, name_)                                               \
  static void mars_body_##suite_##_##name_(int *mm_failures_);               \
  static mars_test_case mars_case_##suite_##_##name_ = {                     \
      #suite_, #name_, mars_body_##suite_##_##name_, NULL};                  \
  __attribute__((constructor)) static void mars_reg_##suite_##_##name_(      \
      void) {                                                                \
    mars_test_register(&mars_case_##suite_##_##name_);                       \
  }                                                                          \
  static void mars_body_##suite_##_##name_(int *mm_failures_)

// --- Failure reporting -----------------------------------------------------

#define MARS_FAIL_(...)                                                      \
  do {                                                                       \
    ++*mm_failures_;                                                         \
    fprintf(stderr, "    %s:%d: ", __FILE__, __LINE__);                      \
    fprintf(stderr, __VA_ARGS__);                                            \
    fputc('\n', stderr);                                                     \
  } while (0)

// --- Integer comparisons ---------------------------------------------------
// Operands are widened to intmax_t, which covers every integer type used here.

#define MARS_CMP_(a_, b_, op_, opstr_, onfail_)                              \
  do {                                                                       \
    intmax_t mars_a_ = (intmax_t)(a_);                                       \
    intmax_t mars_b_ = (intmax_t)(b_);                                       \
    if (!(mars_a_ op_ mars_b_)) {                                            \
      MARS_FAIL_("expected %s %s %s, got %" PRIdMAX " vs %" PRIdMAX, #a_,    \
                 opstr_, #b_, mars_a_, mars_b_);                             \
      onfail_;                                                               \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a_, b_) MARS_CMP_(a_, b_, ==, "==", (void)0)
#define CHECK_NE(a_, b_) MARS_CMP_(a_, b_, !=, "!=", (void)0)
#define CHECK_LT(a_, b_) MARS_CMP_(a_, b_, <, "<", (void)0)
#define CHECK_LE(a_, b_) MARS_CMP_(a_, b_, <=, "<=", (void)0)
#define CHECK_GT(a_, b_) MARS_CMP_(a_, b_, >, ">", (void)0)
#define CHECK_GE(a_, b_) MARS_CMP_(a_, b_, >=, ">=", (void)0)

#define REQUIRE_EQ(a_, b_) MARS_CMP_(a_, b_, ==, "==", return)
#define REQUIRE_NE(a_, b_) MARS_CMP_(a_, b_, !=, "!=", return)

// --- Truth and pointers ----------------------------------------------------

#define CHECK_TRUE(x_)                                                       \
  do {                                                                       \
    if (!(x_)) MARS_FAIL_("expected true: %s", #x_);                         \
  } while (0)

#define CHECK_FALSE(x_)                                                      \
  do {                                                                       \
    if ((x_)) MARS_FAIL_("expected false: %s", #x_);                         \
  } while (0)

#define REQUIRE_TRUE(x_)                                                     \
  do {                                                                       \
    if (!(x_)) {                                                             \
      MARS_FAIL_("expected true: %s", #x_);                                  \
      return;                                                                \
    }                                                                        \
  } while (0)

#define CHECK_NULL(p_)                                                       \
  do {                                                                       \
    const void *mars_p_ = (const void *)(p_);                                \
    if (mars_p_ != NULL) MARS_FAIL_("expected %s == NULL, got %p", #p_,      \
                                    mars_p_);                                \
  } while (0)

#define CHECK_NOT_NULL(p_)                                                   \
  do {                                                                       \
    if ((const void *)(p_) == NULL) MARS_FAIL_("expected %s != NULL", #p_);  \
  } while (0)

#define REQUIRE_NOT_NULL(p_)                                                 \
  do {                                                                       \
    if ((const void *)(p_) == NULL) {                                        \
      MARS_FAIL_("expected %s != NULL", #p_);                                \
      return;                                                                \
    }                                                                        \
  } while (0)

#define CHECK_PTR_EQ(a_, b_)                                                 \
  do {                                                                       \
    const void *mars_pa_ = (const void *)(a_);                               \
    const void *mars_pb_ = (const void *)(b_);                               \
    if (mars_pa_ != mars_pb_)                                                \
      MARS_FAIL_("expected %s == %s, got %p vs %p", #a_, #b_, mars_pa_,      \
                 mars_pb_);                                                  \
  } while (0)

// --- Buffers ---------------------------------------------------------------

#define CHECK_MEM_EQ(a_, b_, n_)                                             \
  do {                                                                       \
    size_t mars_n_ = (size_t)(n_);                                           \
    if (memcmp((a_), (b_), mars_n_) != 0)                                    \
      MARS_FAIL_("%zu bytes at %s differ from %s", mars_n_, #a_, #b_);       \
  } while (0)

#define CHECK_STR_EQ(a_, b_)                                                 \
  do {                                                                       \
    const char *mars_sa_ = (a_);                                             \
    const char *mars_sb_ = (b_);                                             \
    if (strcmp(mars_sa_, mars_sb_) != 0)                                     \
      MARS_FAIL_("expected \"%s\", got \"%s\"", mars_sb_, mars_sa_);         \
  } while (0)

#endif  // MARS_TEST_H_
