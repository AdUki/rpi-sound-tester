#pragma once

// The rates a board clocks the engine at: 48 kHz, where the VIM3L's card runs, and the Pi's
// 96 kHz. A test of anything that depends on the rate runs at both, so that nothing is right at
// 96 kHz only by accident. Both boards run 1024-frame blocks.
inline constexpr unsigned kTestRates[] = {48000, 96000};
inline constexpr unsigned kTestPeriod = 1024;
