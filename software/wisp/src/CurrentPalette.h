// CurrentPalette — the most recently resolved Aurora palette for the zone wisp
// is shadowing. Phase B holds it; later phases pipe these colors to the lamp
// grid as the painting source.
//
// Storage is uint8_t RGBW. Aurora carries two color shapes:
//   - hexColors: 24-bit packed RGB (w=0).
//   - colors[]:  float channels 0..1 (r,g,b,w + amber/uv we ignore).
// The amber + uv channels are dropped — wisp's downstream consumers are the
// lamp grid (RGBW), so chromatic remap of those would be lossy and confusing.
//
// Header is host-portable (no Arduino includes). The .cpp uses millis() at the
// call site, not here.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aurora/PaletteList.h"  // Palette, PaletteColor

namespace wisp {

struct RGBW {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t w = 0;
};

class CurrentPalette {
 public:
  // Replace the held palette with the resolved Aurora colors. `nowMs` is the
  // wall-clock at the time of replacement (caller supplies millis() so the
  // header can stay framework-free).
  void update(const Palette& p, uint32_t nowMs);

  const std::string& paletteId() const { return paletteId_; }
  uint32_t lastChangeMs() const { return lastChangeMs_; }
  const std::vector<RGBW>& colors() const { return colors_; }

 private:
  std::string paletteId_;
  uint32_t lastChangeMs_ = 0;
  std::vector<RGBW> colors_;
};

}  // namespace wisp
