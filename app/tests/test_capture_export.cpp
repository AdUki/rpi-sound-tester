#include "capture_export.h"

#include <iostream>

#include "check.h"

using namespace st;

namespace {

constexpr double kRate = 96000.0;
constexpr uint64_t kMB = 1024 * 1024;

ExportBuffers live_only() {
  ExportBuffers b;
  b.live_begin = 1000000;
  b.live_end = 5000000;
  return b;
}

ExportBuffers frozen() {
  ExportBuffers b = live_only();
  b.frozen = true;
  b.frozen_begin = 3000000;
  b.frozen_end = 3960000;
  return b;
}

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

void test_needs_exactly_one_way_to_name_the_span() {
  ExportQuery q;
  ExportSpan s = resolve_export_span(q, live_only(), kRate, 8, 256 * kMB);
  CHECK_EQ(s.status, 400);
  CHECK(has(s.error, "give start and len"));

  q.start = 2000000;
  s = resolve_export_span(q, live_only(), kRate, 8, 256 * kMB);
  CHECK_EQ(s.status, 400);
  CHECK_EQ(s.error, std::string("start and len go together"));

  q.len = 1000;
  q.seconds = 1.0;
  s = resolve_export_span(q, live_only(), kRate, 8, 256 * kMB);
  CHECK_EQ(s.status, 400);
  CHECK(has(s.error, "not both"));
}

void test_start_len_reads_the_live_ring_unfrozen() {
  ExportQuery q;
  q.start = 2000000;
  q.len = 96000;
  const ExportSpan s = resolve_export_span(q, live_only(), kRate, 8, 256 * kMB);
  CHECK_EQ(s.status, 200);
  CHECK(!s.from_snapshot);
  CHECK_EQ(s.start, 2000000u);
  CHECK_EQ(s.len, 96000u);

  // The whole live ring, exactly, fits; one sample more on either side does not.
  q.start = 1000000;
  q.len = 4000000;
  CHECK_EQ(resolve_export_span(q, live_only(), kRate, 8, 256 * kMB).status, 200);
  q.len = 4000001;
  ExportSpan bad = resolve_export_span(q, live_only(), kRate, 8, 256 * kMB);
  CHECK_EQ(bad.status, 400);
  CHECK_EQ(bad.error, std::string("[1000000, 5000001) is outside the live ring [1000000, 5000000)"));
  q.start = 999999;
  q.len = 10;
  CHECK_EQ(resolve_export_span(q, live_only(), kRate, 8, 256 * kMB).status, 400);
  q.start = 2000000;
  q.len = 0;
  CHECK(has(resolve_export_span(q, live_only(), kRate, 8, 256 * kMB).error, "len must be >= 1"));
  // A huge start (a negative pushed through the u64 parse) fails the range, never wraps.
  q.start = ~0ull - 5;
  q.len = 100;
  CHECK_EQ(resolve_export_span(q, live_only(), kRate, 8, 256 * kMB).status, 400);
}

void test_start_len_reads_the_snapshot_when_frozen() {
  ExportQuery q;
  q.start = 3000000;
  q.len = 960000;
  const ExportSpan s = resolve_export_span(q, frozen(), kRate, 8, 256 * kMB);
  CHECK_EQ(s.status, 200);
  CHECK(s.from_snapshot);
  // Live, but outside the snapshot: refused with the snapshot's range.
  q.start = 2000000;
  q.len = 1000;
  const ExportSpan bad = resolve_export_span(q, frozen(), kRate, 8, 256 * kMB);
  CHECK_EQ(bad.status, 400);
  CHECK(has(bad.error, "outside the frozen range [3000000, 3960000)"));
}

void test_seconds_is_the_live_tail_even_when_frozen() {
  ExportQuery q;
  q.seconds = 2.5;
  for (const ExportBuffers& b : {live_only(), frozen()}) {
    const ExportSpan s = resolve_export_span(q, b, kRate, 8, 256 * kMB);
    CHECK_EQ(s.status, 200);
    CHECK(!s.from_snapshot);
    CHECK_EQ(s.len, 240000u);
    CHECK_EQ(s.start, 5000000u - 240000u);
  }
  q.seconds = 41.7;  // the ring holds 4e6 / 96000 = 41.666 s
  ExportSpan s = resolve_export_span(q, live_only(), kRate, 8, 256 * kMB);
  CHECK_EQ(s.status, 400);
  CHECK(has(s.error, "seconds must be at most 41.666"));
  // The limit the message names is itself accepted.
  q.seconds = 41.666;
  CHECK_EQ(resolve_export_span(q, live_only(), kRate, 8, 256 * kMB).status, 200);
  q.seconds = 0;
  CHECK(has(resolve_export_span(q, live_only(), kRate, 8, 256 * kMB).error, "> 0"));
  q.seconds = -1;
  CHECK_EQ(resolve_export_span(q, live_only(), kRate, 8, 256 * kMB).status, 400);
  q.seconds = 1e-9;
  CHECK_EQ(resolve_export_span(q, live_only(), kRate, 8, 256 * kMB).status, 400);
}

void test_live_margin_keeps_clear_of_the_oldest_sample() {
  ExportBuffers b = live_only();
  b.live_margin = 192000;  // 2 s at 96 kHz
  ExportQuery q;
  q.seconds = 41.0;  // the ring holds 41.666 s, less the 2 s margin
  ExportSpan s = resolve_export_span(q, b, kRate, 8, 256 * kMB);
  CHECK_EQ(s.status, 400);
  CHECK(has(s.error, "seconds must be at most 39.666"));
  CHECK(has(s.error, "less 2.0 s"));
  q.seconds = 39.666;
  CHECK_EQ(resolve_export_span(q, b, kRate, 8, 256 * kMB).status, 200);

  ExportQuery r;
  r.start = 1000000;  // live_oldest itself
  r.len = 1000;
  s = resolve_export_span(r, b, kRate, 8, 256 * kMB);
  CHECK_EQ(s.status, 400);
  CHECK_EQ(s.error, std::string("[1000000, 1001000) is outside the live ring [1192000, 5000000)"));
  r.start = 1192000;
  CHECK_EQ(resolve_export_span(r, b, kRate, 8, 256 * kMB).status, 200);
  // The margin never applies to a frozen snapshot.
  ExportBuffers f = frozen();
  f.live_margin = 192000;
  r.start = 3000000;
  CHECK_EQ(resolve_export_span(r, f, kRate, 8, 256 * kMB).status, 200);
  // A margin wider than the ring leaves nothing, without wrapping.
  b.live_margin = 10000000;
  q.seconds = 0.001;
  CHECK_EQ(resolve_export_span(q, b, kRate, 8, 256 * kMB).status, 400);
}

void test_overflowing_range_says_so() {
  ExportQuery q;
  q.start = 1;
  q.len = ~0ull;
  ExportSpan s = resolve_export_span(q, live_only(), kRate, 8, 256 * kMB);
  CHECK_EQ(s.status, 400);
  CHECK(has(s.error, "start + len is past the end"));
  CHECK(has(s.error, "[1000000, 5000000)"));
  q.start = ~0ull;
  q.len = 1;
  s = resolve_export_span(q, live_only(), kRate, 8, 256 * kMB);
  CHECK_EQ(s.status, 400);
  CHECK(has(s.error, "start + len is past the end"));
}

void test_body_cap() {
  // 16 channels of f32 = 64 bytes a frame: a 1 MB cap holds 16384 frames.
  ExportQuery q;
  q.start = 2000000;
  q.len = 16384;
  CHECK_EQ(resolve_export_span(q, live_only(), kRate, 64, kMB).status, 200);
  q.len = 16385;
  const ExportSpan s = resolve_export_span(q, live_only(), kRate, 64, kMB);
  CHECK_EQ(s.status, 400);
  CHECK(has(s.error, "at most 1 MB, i.e. 16384 frames (0.171 s)"));
  // s16 of the same channels fits twice as much.
  CHECK_EQ(resolve_export_span(q, live_only(), kRate, 32, kMB).status, 200);
}

}  // namespace

int main() {
  test_needs_exactly_one_way_to_name_the_span();
  test_start_len_reads_the_live_ring_unfrozen();
  test_start_len_reads_the_snapshot_when_frozen();
  test_seconds_is_the_live_tail_even_when_frozen();
  test_live_margin_keeps_clear_of_the_oldest_sample();
  test_overflowing_range_says_so();
  test_body_cap();
  return report("capture_export");
}
