#pragma once

#include <cmath>
#include <cstdio>
#include <iostream>
#include <type_traits>

inline int g_failures = 0;

// One pi for every test file; the daemon itself uses st::kTwoPi (util/dsp.h).
inline constexpr double kPi = 3.14159265358979323846;

// Enums (SourceType, GenId, ...) have no operator<<; print them as their underlying value.
template <typename T>
void print_value(const T& v) {
  if constexpr (std::is_enum_v<T>) {
    std::cout << static_cast<long long>(static_cast<std::underlying_type_t<T>>(v));
  } else {
    std::cout << v;
  }
}

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                           \
  do {                                                                           \
    const auto va = (a);                                                         \
    const auto vb = (b);                                                         \
    if (!(va == vb)) {                                                           \
      std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);         \
      std::cout << "  left:  ";                                                  \
      print_value(va);                                                           \
      std::cout << "\n  right: ";                                                \
      print_value(vb);                                                           \
      std::cout << "\n";                                                         \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                    \
  do {                                                                           \
    const double va = static_cast<double>(a);                                    \
    const double vb = static_cast<double>(b);                                    \
    if (!(std::fabs(va - vb) <= (tol))) {                                        \
      std::printf("FAIL %s:%d: |%s - %s| <= %g (got %g vs %g)\n", __FILE__,      \
                  __LINE__, #a, #b, static_cast<double>(tol), va, vb);           \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

inline int report(const char* name) {
  if (g_failures == 0) {
    std::printf("PASS %s\n", name);
    return 0;
  }
  std::printf("FAILED %s: %d check(s)\n", name, g_failures);
  return 1;
}
