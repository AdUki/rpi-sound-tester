#include "util/dsp.h"

#include <cstdint>

#include "check.h"

using namespace st;

namespace {

// The whole point of kS32Scale being 2147483520 rather than 2^31-1: 2^31-1 rounds up to 2^31
// as a float, and casting that back to int32 wraps +1.0 to INT32_MIN on the playback path.
void test_float_to_s32_never_wraps() {
  CHECK_EQ(float_to_s32(1.0f), 2147483520);
  CHECK_EQ(float_to_s32(-1.0f), -2147483520);
  CHECK(float_to_s32(1.0f) > 0);
  CHECK(float_to_s32(2.0f) > 0);   // clamps, does not wrap
  CHECK(float_to_s32(-2.0f) < 0);
  CHECK_EQ(float_to_s32(0.0f), 0);
}

void test_float_to_s16_clamps() {
  CHECK_EQ(float_to_s16(1.0f), 32767);
  CHECK_EQ(float_to_s16(2.0f), 32767);
  CHECK_EQ(float_to_s16(-1.0f), -32767);
  CHECK_EQ(float_to_s16(0.0f), 0);
}

void test_db_conversions_hit_their_floors() {
  CHECK_EQ(db_to_lin(-100.1f), 0.0f);
  CHECK_NEAR(db_to_lin(0.0f), 1.0, 1e-6);
  CHECK_NEAR(db_to_lin(-6.0f), 0.5012, 1e-3);
  CHECK_EQ(lin_to_db(0.0f), kMinDb);
  CHECK_NEAR(lin_to_db(1.0f), 0.0, 1e-5);
}

void test_xorshift_white_stays_in_range() {
  uint64_t state = 0x853c49e6748fea9bull;
  double sum = 0.0;
  for (int i = 0; i < 100000; ++i) {
    const float v = xorshift_white(state);
    CHECK(v >= -1.0f && v < 1.0f);
    sum += v;
  }
  CHECK_NEAR(sum / 100000.0, 0.0, 0.02);  // zero mean
}

}  // namespace

int main() {
  test_float_to_s32_never_wraps();
  test_float_to_s16_clamps();
  test_db_conversions_hit_their_floors();
  test_xorshift_white_stays_in_range();
  return report("dsp");
}
