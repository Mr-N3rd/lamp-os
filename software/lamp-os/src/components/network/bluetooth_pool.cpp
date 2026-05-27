#include "./bluetooth_pool.hpp"

#include <Arduino.h>

#include <cstdint>
#include <string>

#include "../../util/color.hpp"

namespace lamp {
BluetoothPool::BluetoothPool() {
  // Created once at static-init / first use. The mutex protects every
  // pool method from cross-task races (NimBLE host on Core 0 vs Arduino
  // loop on Core 1).
  mutex = xSemaphoreCreateMutex();
}

BluetoothLampRecord::BluetoothLampRecord(std::string inName,
                                         Color inBaseColor,
                                         Color inShadeColor,
                                         uint32_t inLastSeenTimeMs) : name(inName),
                                                                      baseColor(inBaseColor),
                                                                      shadeColor(inShadeColor),
                                                                      lastSeenTimeMs(inLastSeenTimeMs) {};

void BluetoothPool::addLamp(BluetoothLampRecord lamp) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (lampPool.size() < MAX_POOL_SIZE) {
    lampPool.push_back(lamp);
  }
  xSemaphoreGive(mutex);
};

void BluetoothPool::addOrUpdateLamp(BluetoothLampRecord lamp) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  uint32_t timeNow = millis();

  bool updated = false;
  for (size_t i = 0; i < lampPool.size(); i++) {
    if (lampPool[i].name == lamp.name) {
      lampPool[i].lastSeenTimeMs = timeNow;
      // Refresh the colors too — without this, the social color-sync sees
      // only each lamp's boot-time colors, never their live ones.
      lampPool[i].baseColor  = lamp.baseColor;
      lampPool[i].shadeColor = lamp.shadeColor;
      updated = true;
      break;
    }
  }

  if (!updated && lampPool.size() < MAX_POOL_SIZE) {
    lampPool.push_back(lamp);
  }
  xSemaphoreGive(mutex);
};

void BluetoothPool::pruneLamps() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  uint32_t timeNow = millis();

  auto it = lampPool.begin();
  while (it != lampPool.end()) {
    if (it->lastSeenTimeMs + LAMP_PRUNE_TIME_MS < timeNow) {
      it = lampPool.erase(it);
    } else {
      ++it;
    }
  }
  xSemaphoreGive(mutex);
}

std::vector<BluetoothLampRecord> BluetoothPool::getLamps() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  std::vector<BluetoothLampRecord> snapshot = lampPool;  // copy under lock
  xSemaphoreGive(mutex);
  return snapshot;
};

void BluetoothPool::acknowledgeLamp(const std::string& name) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  for (auto& r : lampPool) {
    if (r.name == name) {
      r.acknowledged = true;
      break;
    }
  }
  xSemaphoreGive(mutex);
};

}  // namespace lamp