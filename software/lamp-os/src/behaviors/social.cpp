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
  // Respect the social.enabled flag — skip all greeting logic when disabled
  if (!socialEnabled) return;

  foundLamps = bt->getLamps();

  if (animationState == STOPPED && millis() > nextAcknowledgeTimeMs) {
    for (std::vector<BluetoothLampRecord>::reverse_iterator revIter =
             foundLamps->rbegin();
         revIter != foundLamps->rend(); ++revIter) {
      if (!revIter->acknowledged) {
        // When friends-only mode is on, skip lamps that aren't in the friends list
        if (socialFriendsOnly && !socialFriends.empty()) {
          bool isFriend = false;
          for (const auto& friendName : socialFriends) {
            if (revIter->name == friendName) {
              isFriend = true;
              break;
            }
          }
          if (!isFriend) {
            revIter->acknowledged = true;  // mark as seen so we don't re-check
            continue;
          }
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
  socialEnabled = settings.enabled;
  socialFriendsOnly = settings.friendsOnly;
  socialFriends = settings.friends;
  socialCooldownMs = settings.cooldownMs;
};
}  // namespace lamp