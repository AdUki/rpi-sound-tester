#pragma once

#include <cstddef>
#include <cstdint>

#include "constants.h"
#include "control.h"
#include "generators.h"
#include "util/dsp.h"

namespace st {

// Renders one output channel's block: whatever `oc` routes to it, at its gain and mute, with an
// Identify burst laid over the top while one is running. `dst` is that channel's first sample in
// an interleaved block `DstStride` channels wide.
//
// One function for every sink, so the Octo's eight DACs and the HDMI pair cannot come to disagree
// about what a source, a gain or an Identify means. The stride is a template parameter so each
// caller gets a loop with a compile-time step, which is what the vectorizer needs.
//
// `in_all` is the LIVE input block (kTotalInputs wide), never the capture-axis copy: a passthrough
// must not pick up the alignment delay. `gens` holds one bus per GenId. Audio thread only; every
// control value is read once per block, never per sample, since an atomic load inside the sample
// loop would defeat the vectorizer.
template <size_t DstStride>
inline void route_output(const OutputControl& oc, uint64_t n, size_t frames, const float* in_all,
                         const float* const* gens, const Generators& gen, uint64_t identify_frames,
                         float* dst) {
  const uint32_t packed = oc.source.load(std::memory_order_relaxed);
  const SourceType type = source_type(packed);
  const uint8_t index = source_index(packed);
  const float gain = oc.mute.load(std::memory_order_relaxed)
                         ? 0.0f
                         : db_to_lin(oc.gain_db.load(std::memory_order_relaxed));
  const uint64_t identify_until = oc.identify_until.load(std::memory_order_relaxed);

  const float* src = nullptr;
  size_t stride = 1;
  if (type == SourceType::Input && index < kTotalInputs) {
    src = in_all + index;
    stride = kTotalInputs;
  } else if (type == SourceType::Gen && index < static_cast<uint8_t>(GenId::Count)) {
    src = gens[index];
  }

  if (identify_until > n) {
    // Identify overrides whatever is routed here, then reverts on its own. Rare and brief, so it
    // gets the slow path all to itself.
    for (size_t i = 0; i < frames; ++i) {
      const uint64_t t = n + i;
      dst[i * DstStride] =
          t < identify_until ? gen.identify_sample(identify_frames - (identify_until - t))
          : src              ? gain * src[i * stride]
                             : 0.0f;
    }
  } else if (!src) {
    for (size_t i = 0; i < frames; ++i) dst[i * DstStride] = 0.0f;
  } else {
    for (size_t i = 0; i < frames; ++i) dst[i * DstStride] = gain * src[i * stride];
  }
}

// Whether `oc` currently plays generator `g`.
inline bool routes_gen(const OutputControl& oc, GenId g) {
  return oc.source.load(std::memory_order_relaxed) ==
         pack_source(SourceType::Gen, static_cast<uint8_t>(g));
}

}  // namespace st
