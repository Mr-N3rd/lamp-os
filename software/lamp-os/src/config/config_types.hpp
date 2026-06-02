#ifndef LAMP_CONFIG_CONFIG_TYPES_H
#define LAMP_CONFIG_CONFIG_TYPES_H

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "../util/color.hpp"

namespace lamp {

/**
 * @brief users may selectively dim parts of their lamp to deal with difficult
 *        led positions like the neck of a vase base that may show the pixels
 *        too sharply
 * @property p - the pixel index to dim. indexed from 0
 * @property b - the brightness level as a percentage
 */
class KnockoutPixel {
 public:
  uint8_t p;
  uint8_t b;
};

/**
 * @brief Global lamp settings to control initialization
 * @property name - a name that can be used to identify this lamp. it can be up to 12 characters long
 * @property brightness - global brightness level for the lamp as a percentage
 * @property password - password to protect lamp BLE control surface
 * @property advancedEnabled - if true, advanced settings UI is unlocked
 */
class LampSettings {
 public:
  std::string name = "standard";
  uint8_t brightness = 100;
  std::string password = "";
  bool advancedEnabled = false;
};

/**
 * @brief Settings used for the bulb neopixels
 * @property px - the total pixel count
 * @property bpp - bytes per pixel: 4 = RGBW-class strip, 3 = RGB-class strip
 * @property byteOrder - NeoPixel wire byte order. Recognized values:
 *   "GRBW" (4 bpp), "GRB" (3 bpp), "BGR" (3 bpp). Empty string falls back
 *   to a bpp-derived default ("GRBW" for bpp==4, "GRB" otherwise) so old
 *   NVS payloads without the field load unchanged.
 * @property colors - a list of up to 5 colors to use
 */
class ShadeSettings {
 public:
  uint8_t px = 38;
  uint8_t bpp = 4;
  std::string byteOrder = "";
  std::vector<Color> colors = {Color(0x00, 0x00, 0x00, 0xFF)};
};

/**
 * @brief Settings used for the base neopixels
 * @property px - the total pixel count
 * @property bpp - bytes per pixel: 4 = RGBW-class strip, 3 = RGB-class strip
 * @property byteOrder - NeoPixel wire byte order; see ShadeSettings.
 * @property colors - a list of up to 5 colors to use
 * @property knockoutPixels - a list of knockout pixels to profile the lamp base
 * @property ac - the preferred color index in a gradient
 */
class BaseSettings {
 public:
  uint8_t px = 35;
  uint8_t bpp = 4;
  std::string byteOrder = "";
  std::vector<Color> colors = {Color(0x30, 0x07, 0x83, 0x00)};
  std::vector<uint8_t> knockoutPixels = std::vector<uint8_t>(50, (uint8_t)100);
  uint8_t ac = 0;
};

/**
 * @brief Configuration for a single expression with generic parameter system
 */
class ExpressionConfig {
 public:
  std::string type = "";           // Expression type (e.g., "glitchy", "shifty")
  bool enabled = false;            // Whether expression is active
  std::vector<Color> colors;       // Color palette for expression
  uint32_t intervalMin = 60;       // Min interval in seconds
  uint32_t intervalMax = 900;      // Max interval in seconds
  uint8_t target = 3;              // TARGET_SHADE=1, TARGET_BASE=2, TARGET_BOTH=3

  // Generic parameter storage for expression-specific values
  std::map<std::string, uint32_t> parameters;

  // Helper methods for parameter access
  uint32_t getParameter(const std::string& name, uint32_t defaultValue) const {
    auto it = parameters.find(name);
    if (it != parameters.end()) {
      return it->second;
    }
    return defaultValue;
  }

  void setParameter(const std::string& name, uint32_t value) {
    parameters[name] = value;
  }
};

/**
 * @brief Settings for lamp expressions
 */
class ExpressionSettings {
 public:
  std::vector<ExpressionConfig> expressions;
};

/**
 * @brief Home mode: presence-only detection of the user's home WiFi.
 *        The lamp NEVER associates to the AP — it just sniffs beacons
 *        and treats the SSID being visible as "I'm at home". No password
 *        is ever stored or transmitted (security: no MITM risk against
 *        a hostile AP broadcasting the same SSID).
 *
 *        While in home mode the lamp uses `brightness` (this struct's)
 *        instead of LampSettings.brightness, and the compositor pauses
 *        behaviors flagged allowedInHomeMode=false (e.g. SocialBehavior).
 *
 * @property ssid       the SSID to watch for. Set via wifi_op setHomeSsid
 *                      (which the app sends when the user taps an entry
 *                      from a scan list). Empty = no home network.
 * @property brightness brightness to use when home mode is active.
 * @property enabled    soft on/off. When false the lamp ignores SSID
 *                      visibility and stays in regular mode.
 */
class HomeModeSettings {
 public:
  std::string ssid;
  uint8_t brightness = 60;
  bool enabled = false;
};

}  // namespace lamp
#endif