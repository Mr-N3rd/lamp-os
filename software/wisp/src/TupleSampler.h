// TupleSampler — pure function from (palette, mac) → two colors.
//
// Phase C.4: when wisp paints the mesh, every peer gets two colors sampled at
// MAC-hashed positions along the active Aurora gradient. Same MAC always
// produces the same pair, so the lamp's two surfaces (base + shade) stay
// stable across repeated paints until either the palette changes or the
// lamp's MAC changes (i.e. never, during a session).
//
// Host-portable: no Arduino, no FreeRTOS. The .cpp is included in the native
// build via build_src_filter, and the test fixture redeclares the algorithm
// locally for full isolation.
//
// Algorithm (see plan §"Tuple sampler + mesh paint"):
//   1. Normalize the gradient: dedupe near-identical neighbors, interpolate
//      up to at least 4 stops so single/double-color palettes still give
//      varied sample positions a useful gradient to hit.
//   2. Hash 1: a = (fnv1a(mac) % 1000) / 1000 → position on [0,1].
//   3. Hash 2: b = (fnv1a(mac XOR 0x9E3779B9) % 1000) / 1000.
//   4. If |a - b| < 0.15 re-roll b up to 3 times (XOR with rotated constant)
//      to avoid near-identical pairings.
//   5. Sample the normalized gradient at a and b via linear interpolation.

#pragma once

#include <cstddef>
#include <cstdint>

#include "CurrentPalette.h"

namespace wisp {

// Two RGBW colors. Indexed [0] / [1]; some callers map [0] → base surface,
// [1] → shade surface. The wisp itself doesn't take an opinion — receivers
// decide which surface gets which color via MSG_OVERRIDE_COLORS surface byte.
struct ColorTuple {
  uint8_t r[2] = {0, 0};
  uint8_t g[2] = {0, 0};
  uint8_t b[2] = {0, 0};
  uint8_t w[2] = {0, 0};
};

// Pure function. Determinism property: same `palette.colors()` snapshot +
// same `mac` always returns the same tuple. Edge cases:
//   - Empty palette → {0,0,0,0} for both colors.
//   - Single-color palette → that color for both positions.
//   - 2-3 colors → interpolated to 4 stops before sampling.
ColorTuple sampleTupleForMac(const CurrentPalette& palette,
                             const uint8_t mac[6]);

}  // namespace wisp
