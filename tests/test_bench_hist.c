// The latency histogram decides every percentile the harness reports, so its
// accuracy is worth pinning independently of anything it is used to measure.

#include "mars_test.h"

#include <stdlib.h>

#include "bench_timer.h"

// Buckets are log-linear with 128 sub-buckets per octave, so a reported value
// may sit up to one sub-bucket below the true one. Under 1% for any value
// above the linear region.
static int within_tolerance(uint64_t reported, uint64_t truth) {
  if (reported == truth) return 1;
  if (reported > truth) return 0;  // quantiles report the bucket floor
  double err = (double)(truth - reported) / (double)truth;
  return err <= 0.01;
}

MM_TEST(hist, small_values_are_recorded_exactly) {
  bench_hist h;
  bench_hist_reset(&h);
  for (uint64_t v = 0; v < 128; v++) bench_hist_record(&h, v);

  CHECK_EQ(h.count, 128);
  CHECK_EQ(h.min, 0);
  CHECK_EQ(h.max, 127);
  // Below the sub-bucket count every value has its own slot. Half of 128
  // samples is the 64th smallest, which for values 0..127 is 63.
  CHECK_EQ(bench_hist_quantile(&h, 0.50), 63);
}

MM_TEST(hist, quantiles_of_a_uniform_distribution) {
  bench_hist h;
  bench_hist_reset(&h);
  for (uint64_t v = 1; v <= 100000; v++) bench_hist_record(&h, v);

  CHECK_EQ(h.count, 100000);
  CHECK_EQ(h.max, 100000);

  struct { double q; uint64_t want; } cases[] = {
      {0.50, 50000}, {0.90, 90000}, {0.99, 99000}, {0.999, 99900}};

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    uint64_t got = bench_hist_quantile(&h, cases[i].q);
    if (!within_tolerance(got, cases[i].want)) {
      MARS_FAIL_("q=%.3f reported %llu, expected about %llu", cases[i].q,
                 (unsigned long long)got, (unsigned long long)cases[i].want);
    }
  }
}

MM_TEST(hist, a_single_outlier_does_not_drag_the_median) {
  bench_hist h;
  bench_hist_reset(&h);
  for (int i = 0; i < 9999; i++) bench_hist_record(&h, 100);
  bench_hist_record(&h, 100000000);

  CHECK_EQ(h.max, 100000000);
  // The median must stay with the bulk of the samples.
  CHECK_TRUE(within_tolerance(bench_hist_quantile(&h, 0.50), 100));
  CHECK_TRUE(within_tolerance(bench_hist_quantile(&h, 0.99), 100));
}

MM_TEST(hist, extremes_are_reported_as_the_true_min_and_max) {
  bench_hist h;
  bench_hist_reset(&h);
  bench_hist_record(&h, 7);
  bench_hist_record(&h, 999999999);

  CHECK_EQ(bench_hist_quantile(&h, 0.0), 7);
  CHECK_EQ(bench_hist_quantile(&h, 1.0), 999999999);
  // A quantile must never exceed the largest value actually seen.
  CHECK_LE(bench_hist_quantile(&h, 0.999), h.max);
}

MM_TEST(hist, an_empty_histogram_reports_zero) {
  bench_hist h;
  bench_hist_reset(&h);
  CHECK_EQ(h.count, 0);
  CHECK_EQ(bench_hist_quantile(&h, 0.5), 0);
  CHECK_EQ(bench_hist_mean(&h), 0);
}

MM_TEST(hist, mean_matches_a_hand_computed_average) {
  bench_hist h;
  bench_hist_reset(&h);
  for (uint64_t v = 1; v <= 1000; v++) bench_hist_record(&h, v);
  // The mean is accumulated from the raw values, not the buckets, so it is
  // exact.
  CHECK_EQ((uint64_t)bench_hist_mean(&h), 500);
}

MM_TEST(hist, the_timer_reports_a_plausible_calibration) {
  const bench_timer_info *t = bench_timer_init();
  REQUIRE_NOT_NULL(t);

  // Anything outside this range means calibration went wrong rather than that
  // the machine is unusual: it spans roughly 100 MHz to 100 GHz.
  CHECK_TRUE(t->ns_per_tick > 0.0);
  CHECK_TRUE(t->ns_per_tick < 10.0);
  CHECK_TRUE(t->overhead_ns >= 0.0);
  CHECK_TRUE(t->overhead_ns < 10000.0);

  // Converting a delta must subtract the timer's own cost and never go
  // negative.
  CHECK_EQ(bench_ticks_to_ns(0), 0.0);

  // A measured interval should land near what the wall clock says.
  uint64_t w0 = bench_wall_ns();
  uint64_t t0 = bench_ticks();
  while (bench_wall_ns() - w0 < 5000000) { /* 5 ms */ }
  uint64_t ticks = bench_ticks() - t0;
  uint64_t wall = bench_wall_ns() - w0;

  double measured = bench_ticks_to_ns(ticks);
  double ratio = measured / (double)wall;
  if (ratio < 0.8 || ratio > 1.25) {
    MARS_FAIL_("tick-derived %.0f ns vs wall %llu ns (ratio %.3f)", measured,
               (unsigned long long)wall, ratio);
  }
}
