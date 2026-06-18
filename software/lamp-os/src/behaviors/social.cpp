#include "./social.hpp"

#include <Arduino.h>

#include "../components/network/bluetooth.hpp"
#include "../util/color.hpp"
#include "../util/fade.hpp"

namespace lamp {
bool SocialBehavior::isFriendLamp(const BluetoothLampRecord& lamp) const {
  for (const auto& friendName : socialFriends) {
    if (lamp.name == friendName) {
      return true;
    }
  }

  return false;
}

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
  if (socialMode == SocialMode::OFF) return;

  foundLamps = bt->getLamps();

  if (animationState == STOPPED && millis() > nextAcknowledgeTimeMs) {
    for (std::vector<BluetoothLampRecord>::reverse_iterator revIter =
             foundLamps->rbegin();
         revIter != foundLamps->rend(); ++revIter) {
      if (!revIter->acknowledged) {
        if (socialMode == SocialMode::SHY && !isFriendLamp(*revIter)) {
          continue;
        }

#ifdef LAMP_DEBUG
        Serial.printf("Acknowledging %s\n", revIter->name.c_str());
#endif
        revIter->acknowledged = true;
        foundLampColor = revIter->baseColor;
        // Use config-driven cooldown instead of the former compile-time constant
        nextAcknowledgeTimeMs = millis() + socialCooldownMs;

        playOnce();
        break;
      }
    }
  }
};

void SocialBehavior::setBluetoothComponent(BluetoothComponent* inBt) {
  bt = inBt;
};

void SocialBehavior::configure(const SocialSettings& settings) {
  if (settings.mode == "off") {
    socialMode = SocialMode::OFF;
  } else if (settings.mode == "shy") {
    socialMode = SocialMode::SHY;
  } else if (settings.mode == "butterfly") {
    socialMode = SocialMode::BUTTERFLY;
  } else {
    socialMode = SocialMode::GREET;
  }

  socialFriends = settings.friends;
  socialCooldownMs = settings.cooldownMs;
};
}  // namespace lamp