#include "TupleSampler.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wisp {

namespace {

// FNV-1a 32-bit. Tiny, fast, inlinable. Good enough for "spread MACs across
// a [0,1) line"; we are NOT depending on it for security.
uint32_t fnv1a(const uint8_t* bytes, size_t n) {
  uint32_t h = 0x811C9DC5u;
  for (size_t i = 0; i < n; ++i) {
    h ^= static_cast<uint32_t>(bytes[i]);
    h *= 0x01000193u;
  }
  return h;
}

// Hash a MAC with an optional 32-bit salt XORed in before hashing. The salt
// lets us derive uncorrelated positions for the same MAC (golden-ratio
// constant + per-reroll rotations).
uint32_t hashMac(const uint8_t mac[6], uint32_t salt) {
  uint8_t buf[6];
  // XOR the salt across the MAC bytes in a stable pattern. Don't just hash
  // the MAC and XOR the result — that under-mixes; running the bytes through
  // FNV with the salt baked in spreads the entropy across the full state.
  buf[0] = mac[0] ^ static_cast<uint8_t>(salt & 0xFF);
  buf[1] = mac[1] ^ static_cast<uint8_t>((salt >> 8) & 0xFF);
  buf[2] = mac[2] ^ static_cast<uint8_t>((salt >> 16) & 0xFF);
  buf[3] = mac[3] ^ static_cast<uint8_t>((salt >> 24) & 0xFF);
  buf[4] = mac[4] ^ static_cast<uint8_t>(salt & 0xFF);
  buf[5] = mac[5] ^ static_cast<uint8_t>((salt >> 16) & 0xFF);
  return fnv1a(buf, 6);
}

// Map a hash to a position in [0,1) with three decimal digits of resolution.
float hashToPosition(uint32_t h) {
  return static_cast<float>(h % 1000u) / 1000.0f;
}

bool nearlyEqualColor(const RGBW& a, const RGBW& b) {
  // ~8/255 per channel — about the smallest step the lamp eye can perceive
  // on the WS2812 family. If neighbors are this close, treat them as one.
  constexpr int kDeltaThreshold = 8;
  auto chDelta = [](uint8_t x, uint8_t y) {
    return std::abs(static_cast<int>(x) - static_cast<int>(y));
  };
  return chDelta(a.r, b.r) < kDeltaThreshold &&
         chDelta(a.g, b.g) < kDeltaThreshold &&
         chDelta(a.b, b.b) < kDeltaThreshold &&
         chDelta(a.w, b.w) < kDeltaThreshold;
}

uint8_t lerpChannel(uint8_t a, uint8_t b, float t) {
  // Clamp inside the function so callers don't have to. lroundf would pull
  // <math.h>; a +0.5f cast is plenty for byte interpolation.
  if (t <= 0.0f) return a;
  if (t >= 1.0f) return b;
  float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
  if (v < 0.0f) v = 0.0f;
  if (v > 255.0f) v = 255.0f;
  return static_cast<uint8_t>(v + 0.5f);
}

RGBW lerpColor(const RGBW& a, const RGBW& b, float t) {
  RGBW out;
  out.r = lerpChannel(a.r, b.r, t);
  out.g = lerpChannel(a.g, b.g, t);
  out.b = lerpChannel(a.b, b.b, t);
  out.w = lerpChannel(a.w, b.w, t);
  return out;
}

// Step 1: produce a normalized stop list of length >= 4 with no two
// adjacent stops near-identical. Doesn't sort by hue (Aurora's order is
// the authoring intent we want to preserve); just dedupes runs and pads
// out if needed.
std::vector<RGBW> normalize(const std::vector<RGBW>& in) {
  std::vector<RGBW> out;
  out.reserve(in.size() + 4);

  // Dedupe adjacent near-identical stops.
  for (const auto& c : in) {
    if (out.empty() || !nearlyEqualColor(out.back(), c)) {
      out.push_back(c);
    }
  }

  if (out.size() >= 4) return out;
  if (out.empty()) return out;

  // Pad to 4 by linearly subdividing the existing stops. If we have one
  // stop, duplicate it; the caller handles that as a flat sample. If we
  // have 2-3 stops, generate intermediate stops so MAC-hash positions hit
  // a real gradient instead of clamping to one of two colors.
  if (out.size() == 1) {
    // Single color: replicate so sampler returns the same color anywhere.
    while (out.size() < 4) out.push_back(out[0]);
    return out;
  }

  std::vector<RGBW> expanded;
  expanded.reserve(8);
  expanded.push_back(out.front());
  for (size_t i = 0; i + 1 < out.size(); ++i) {
    expanded.push_back(lerpColor(out[i], out[i + 1], 0.5f));
    expanded.push_back(out[i + 1]);
  }
  // After one pass on 2 stops we have 3; on 3 we have 5. Run another pass
  // for the 2-stop case so we end with at least 4.
  if (expanded.size() < 4) {
    std::vector<RGBW> again;
    again.reserve(8);
    again.push_back(expanded.front());
    for (size_t i = 0; i + 1 < expanded.size(); ++i) {
      again.push_back(lerpColor(expanded[i], expanded[i + 1], 0.5f));
      again.push_back(expanded[i + 1]);
    }
    return again;
  }
  return expanded;
}

// Sample a normalized stop list at position p in [0,1] via linear interp.
// Caller guarantees stops.size() >= 1.
RGBW sampleAt(const std::vector<RGBW>& stops, float p) {
  if (stops.size() == 1) return stops[0];
  if (p <= 0.0f) return stops.front();
  if (p >= 1.0f) return stops.back();
  // N stops divide [0,1] into N-1 segments of equal width.
  float scaled = p * static_cast<float>(stops.size() - 1);
  size_t lo = static_cast<size_t>(scaled);
  if (lo >= stops.size() - 1) lo = stops.size() - 2;
  float t = scaled - static_cast<float>(lo);
  return lerpColor(stops[lo], stops[lo + 1], t);
}

}  // namespace

ColorTuple sampleTupleForMac(const CurrentPalette& palette,
                             const uint8_t mac[6]) {
  ColorTuple out;
  if (!mac) return out;

  const auto& raw = palette.colors();
  if (raw.empty()) return out;

  auto stops = normalize(raw);
  if (stops.empty()) return out;

  // Position A: straight FNV-1a over the MAC.
  const float a = hashToPosition(hashMac(mac, 0u));

  // Position B: salted with the golden ratio constant. The XOR-with-golden-
  // ratio trick is the trick boost::hash_combine uses to decorrelate; same
  // idea here, two outputs from one input.
  constexpr uint32_t kGolden = 0x9E3779B9u;
  float b = hashToPosition(hashMac(mac, kGolden));

  // Re-roll b up to 3 times if too close. Each retry rotates the salt
  // bitwise so we get a fresh sample space; if still no luck after 3 tries,
  // shift b half a turn to guarantee separation (visual jank is worse than
  // theoretical determinism here — the determinism still holds because the
  // shift is deterministic given the original b).
  constexpr float kMinSeparation = 0.15f;
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (std::abs(a - b) >= kMinSeparation) break;
    // Rotate the salt left by (attempt+1)*7 bits then XOR in the original
    // golden constant to keep it deterministic per MAC + attempt.
    uint32_t shift = static_cast<uint32_t>((attempt + 1) * 7) & 31u;
    uint32_t salt = (kGolden << shift) | (kGolden >> (32u - shift));
    salt ^= kGolden;
    b = hashToPosition(hashMac(mac, salt));
  }
  if (std::abs(a - b) < kMinSeparation) {
    // Final fallback: shift b half a turn. Wraps within [0,1).
    b = b + 0.5f;
    if (b >= 1.0f) b -= 1.0f;
  }

  RGBW cA = sampleAt(stops, a);
  RGBW cB = sampleAt(stops, b);

  out.r[0] = cA.r; out.g[0] = cA.g; out.b[0] = cA.b; out.w[0] = cA.w;
  out.r[1] = cB.r; out.g[1] = cB.g; out.b[1] = cB.b; out.w[1] = cB.w;
  return out;
}

}  // namespace wisp
