#include "./social.hpp"

#include <Arduino.h>

#include "../components/network/nearby_lamps.hpp"
#include "../util/color.hpp"
#include "../util/fade.hpp"

namespace lamp {
void SocialBehavior::draw() {
  for (int i = 0; i < fb->pixelCount; i++) {
    if (frame < easeFrames) {
      fb->buffer[i] = fade(fb->buffer[i], foundLampColor, easeFrames - 1, frame);
    } else if (frame > (frames - easeFrames)) {
      fb->buffer[i] = fade(foundLampColor, fb->buffer[i], easeFrames - 1, frame % easeFrames);
    } else {
      fb->buffer[i] = foundLampColor;
    }
  }

  nextFrame();
};

void SocialBehavior::control() {
  // Greet only lamps we've actually heard within BLE-adv range — ESP-NOW-only
  // (potentially 200 m away) shouldn't trigger the close-range social fade.
  // Snapshot is taken under nearbyLamps' mutex so iterating is safe against
  // the NimBLE scan task and the ESP-NOW recv task both writing.
  std::vector<NearbyLamp> foundLamps = nearbyLamps.getReachableViaBle(LAMP_PRUNE_TIME_MS);

  if (animationState == STOPPED && millis() > nextAcknowledgeTimeMs) {
    for (auto it = foundLamps.rbegin(); it != foundLamps.rend(); ++it) {
      if (!it->acknowledged) {
#ifdef LAMP_DEBUG
        Serial.printf("Acknowledging %s\n", it->name.c_str());
#endif
        // Persist back to the live store; the local `it` is just a copy.
        nearbyLamps.acknowledge(it->name);
        foundLampColor = it->baseColor;
        nextAcknowledgeTimeMs = millis() + LAMP_TIME_BETWEEN_ACKNOWLEDGEMENT_MS;

        playOnce();
        break;
      }
    }
  }
};

}  // namespace lamp
