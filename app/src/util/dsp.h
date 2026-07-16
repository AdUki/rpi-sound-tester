#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace st {

inline constexpr float kMinDb = -120.0f;
inline constexpr double kTwoPi = 6.283185307179586476925286766559;

inline float db_to_lin(float db) { return db <= -100.0f ? 0.0f : std::pow(10.0f, db / 20.0f); }

inline float lin_to_db(float lin) {
  return lin <= 1e-6f ? kMinDb : std::max(kMinDb, 20.0f * std::log10(lin));
}

inline float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

inline int16_t float_to_s16(float v) {
  const float x = clampf(v, -1.0f, 1.0f) * 32767.0f;
  return static_cast<int16_t>(std::lrintf(x));
}

// 2^31-1 is not representable as a float (it rounds up to 2^31 and wraps on cast), so
// scale by the largest float strictly below 2^31. The 127-LSB shortfall is 186 dB down --
// far below the codec's 24-bit floor -- and this keeps the conversion a pure float op the
// vectorizer can put in NEON registers.
inline constexpr float kS32Scale = 2147483520.0f;

inline int32_t float_to_s32(float v) {
  return static_cast<int32_t>(clampf(v, -1.0f, 1.0f) * kS32Scale);
}

inline float s32_to_float(int32_t v) { return static_cast<float>(v) * (1.0f / 2147483648.0f); }

// xorshift64* white noise; take the top 24 bits so the value maps exactly onto a float
// mantissa. Shared by the noise generator and the simulator's ADC-noise floor.
inline float xorshift_white(uint64_t& state) {
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  const uint64_t x = state * 0x2545f4914f6cdd1dull;
  return static_cast<float>(x >> 40) * (1.0f / 8388608.0f) - 1.0f;  // [-1, 1)
}

}  // namespace st
