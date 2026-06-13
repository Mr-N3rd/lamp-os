#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <functional>
#include <string>

#include "../../config/config.hpp"

namespace lamp {

/**
 * @brief MQTT broker + Home Assistant integration.
 *
 * Only does work while `config->mqtt.enabled` and `wifi::isConnected()`.
 * BLE-connect already pauses WiFi (coexistence), so MQTT naturally
 * suspends whenever the user is in the mobile app and resumes on
 * disconnect — no additional coordination needed.
 *
 * Publishes HA MQTT discovery for the lamp as a Light entity with
 * brightness, subscribes to the command topic, and calls back to the
 * caller when HA changes brightness or power state.
 */
class MqttComponent {
 public:
  MqttComponent();

  /**
   * @param onBrightness  Invoked when HA sets brightness (0-100).
   * @param onPower       Invoked when HA toggles power on/off.
   */
  void begin(Config* config,
             std::function<void(uint8_t)> onBrightness,
             std::function<void(bool)> onPower);

  /**
   * @brief Drive MQTT connect/reconnect/loop. Call once per loop()
   *        iteration, after `wifi::tick()`. No-op while disabled or while
   *        WiFi isn't STA-connected.
   */
  void tick();

  /**
   * @brief Push current power/brightness state to HA. Call when the local
   *        UI (or BLE write) changes them so HA UI stays in sync.
   */
  void publishState();

  /**
   * @brief Re-read `config->mqtt` and reconnect to the broker with the new
   *        settings. Call after a CHAR_MQTT_OP drain has updated config.
   */
  void applyConfig();

 private:
  Config* config = nullptr;
  WiFiClient wifiClient;
  PubSubClient mqttClient;

  std::function<void(uint8_t)> brightnessCallback;
  std::function<void(bool)> powerCallback;

  bool brokerConfigured = false;
  bool powerState = true;
  unsigned long lastReconnectAttemptMs = 0;
  std::string lampId;
  std::string commandTopic;
  std::string stateTopic;
  std::string availabilityTopic;
  std::string discoveryTopic;

  void configureBroker();      // set server, callback, build topics from current config
  void connectBroker();        // attempt PubSubClient.connect with LWT
  void publishDiscovery();     // HA MQTT discovery
  void publishAvailability(bool online);
  static void messageCallback(char* topic, byte* payload, unsigned int length);
  void handleMessage(const char* payload, unsigned int length);
};

}  // namespace lamp
