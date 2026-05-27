#include "./social.hpp"

#include <Arduino.h>

#include "../components/network/bluetooth.hpp"
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
  // Snapshot the pool under the BluetoothPool mutex. Iterating a live
  // pool while the NimBLE scan task mutates it on Core 0 was the cause of
  // _invalid_pc_placeholder crashes (vector iterator invalidation).
  std::vector<BluetoothLampRecord> foundLamps = bt->getLamps();

  if (animationState == STOPPED && millis() > nextAcknowledgeTimeMs) {
    for (auto it = foundLamps.rbegin(); it != foundLamps.rend(); ++it) {
      if (!it->acknowledged) {
#ifdef LAMP_DEBUG
        Serial.printf("Acknowledging %s\n", it->name.c_str());
#endif
        // Persist the acknowledgement back to the live pool through the
        // locked API; the local `it` is just a snapshot copy.
        bt->acknowledgeLamp(it->name);
        foundLampColor = it->baseColor;
        nextAcknowledgeTimeMs = millis() + LAMP_TIME_BETWEEN_ACKNOWLEDGEMENT_MS;

        playOnce();
        break;
      }
    }
  }
};

void SocialBehavior::setBluetoothComponent(BluetoothComponent* inBt) {
  bt = inBt;
};
}  // namespace lamp