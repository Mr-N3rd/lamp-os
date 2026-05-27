#ifndef LAMP_COMPONENTS_NETWORK_BLUETOOTH_POOL_H
#define LAMP_COMPONENTS_NETWORK_BLUETOOTH_POOL_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../../util/color.hpp"

// Max lamp pool size
#define MAX_POOL_SIZE 20

// Prune lamps after 120 seconds of no bluetooth updates
#define LAMP_PRUNE_TIME_MS 120000

namespace lamp {
/**
 * @brief Generic record for lamps found over Bluetooth
 */
class BluetoothLampRecord {
 public:
  std::string name;
  Color baseColor = Color();
  Color shadeColor = Color();
  uint32_t lastSeenTimeMs;
  bool acknowledged = false;

  BluetoothLampRecord(std::string inName, Color inBaseColor, Color inShadeColor, uint32_t inTimeFoundMs);
};

/**
 * @brief Thread-safe pool of nearby lamp records.
 * Mutated from NimBLE host task (Core 0, via ScanCallbacks) and read from
 * the Arduino loop task (Core 1, via SocialBehavior). Mutex serialises
 * vector::erase against concurrent iteration.
 */
class BluetoothPool {
 public:
  BluetoothPool();

  std::vector<BluetoothLampRecord> lampPool;

  void addLamp(BluetoothLampRecord lamp);
  void addOrUpdateLamp(BluetoothLampRecord lamp);
  std::vector<BluetoothLampRecord> getLamps();
  void pruneLamps();
  void acknowledgeLamp(const std::string& name);

 private:
  SemaphoreHandle_t mutex = nullptr;
};
}  // namespace lamp
#endif