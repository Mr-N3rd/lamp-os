#include "CurrentPalette.h"

#include <algorithm>
#include <cmath>

namespace wisp {

namespace {

uint8_t floatToByte(float v) {
  // Clamp to [0,1] then scale. lroundf rounds half-away-from-zero, which is
  // close enough for visual data and avoids the down-bias of static_cast.
  if (v <= 0.0f) return 0;
  if (v >= 1.0f) return 255;
  long n = lroundf(v * 255.0f);
  if (n < 0) return 0;
  if (n > 255) return 255;
  return static_cast<uint8_t>(n);
}

}  // namespace

void CurrentPalette::update(const Palette& p, uint32_t nowMs) {
  paletteId_ = p.id;
  lastChangeMs_ = nowMs;
  colors_.clear();

  // hexColors is the simpler shape (24-bit packed RGB). When present we use
  // it directly with w=0; this is how the read-only built-in palettes arrive.
  if (!p.hexColors.empty()) {
    colors_.reserve(p.hexColors.size());
    for (uint64_t hex : p.hexColors) {
      const uint32_t v = static_cast<uint32_t>(hex);
      RGBW c;
      c.r = static_cast<uint8_t>((v >> 16) & 0xFF);
      c.g = static_cast<uint8_t>((v >> 8) & 0xFF);
      c.b = static_cast<uint8_t>(v & 0xFF);
      c.w = 0;
      colors_.push_back(c);
    }
    return;
  }

  // colors[] carries float channels. amber and uv are intentionally dropped —
  // wisp paints the lamp grid in RGBW and has no place for those channels.
  colors_.reserve(p.colors.size());
  for (const auto& src : p.colors) {
    RGBW c;
    c.r = floatToByte(src.r);
    c.g = floatToByte(src.g);
    c.b = floatToByte(src.b);
    c.w = floatToByte(src.w);
    colors_.push_back(c);
  }
}

}  // namespace wisp
