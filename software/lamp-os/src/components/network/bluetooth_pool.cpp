#include "./bluetooth_pool.hpp"

#include <Arduino.h>

#include <cstdint>
#include <string>

#include "../../util/color.hpp"

namespace lamp {
BluetoothLampRecord::BluetoothLampRecord(std::string inName,
                                         Color inBaseColor,
                                         Color inShadeColor,
                                         uint32_t inLastSeenTimeMs) : name(inName),
                                                                      baseColor(inBaseColor),
                                                                      shadeColor(inShadeColor),
                                                                      lastSeenTimeMs(inLastSeenTimeMs) {};

void BluetoothPool::addLamp(BluetoothLampRecord lamp) {
  if (lampPool.size() < MAX_POOL_SIZE) {
    lampPool.push_back(lamp);
  }
};

void BluetoothPool::addOrUpdateLamp(BluetoothLampRecord lamp) {
  uint32_t timeNow = millis();

  for (size_t i = 0; i < lampPool.size(); i++) {
    if (lampPool[i].name == lamp.name) {
      lampPool[i].lastSeenTimeMs = timeNow;
      // Refresh the colors too — without this, the social color-sync sees
      // only each lamp's boot-time colors, never their live ones.
      lampPool[i].baseColor  = lamp.baseColor;
      lampPool[i].shadeColor = lamp.shadeColor;
      return;
    }
  }

  addLamp(lamp);
};

void BluetoothPool::pruneLamps() {
  uint32_t timeNow = millis();

  // Iterator-based erase: erase() returns the iterator to the next element,
  // so we don't skip the post-erase shifted neighbor (which the index-based
  // pattern did).
  auto it = lampPool.begin();
  while (it != lampPool.end()) {
    if (it->lastSeenTimeMs + LAMP_PRUNE_TIME_MS < timeNow) {
      it = lampPool.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<BluetoothLampRecord> BluetoothPool::getLamps() { return lampPool; };

BluetoothStageRecord::BluetoothStageRecord(std::string inName,
                                           String inSsid,
                                           String inPassword,
                                           uint32_t inLastSeenTimeMs) {
  name = inName;
  ssid = inSsid;
  password = inPassword;
  lastSeenTimeMs = inLastSeenTimeMs;
};

void BluetoothPool::addStage(BluetoothStageRecord stage) {
  if (stagePool.size() < MAX_POOL_SIZE) {
    stagePool.push_back(stage);
  }
};

void BluetoothPool::addOrUpdateStage(BluetoothStageRecord stage) {
  uint32_t timeNow = millis();

  for (int i = 0; i < stagePool.size(); i++) {
    if (stagePool[i].name == stage.name) {
      stagePool[i].lastSeenTimeMs = timeNow;
      return;
    }
  }

  addStage(stage);
};

void BluetoothPool::pruneStages() {
  uint32_t timeNow = millis();

  // Iterator-based erase — see pruneLamps for rationale.
  auto it = stagePool.begin();
  while (it != stagePool.end()) {
    if (it->lastSeenTimeMs + STAGE_PRUNE_TIME_MS < timeNow) {
      it = stagePool.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<BluetoothStageRecord> BluetoothPool::getStages() { return stagePool; };
}  // namespace lamp