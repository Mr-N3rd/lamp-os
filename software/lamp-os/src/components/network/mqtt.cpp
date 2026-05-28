#include "./mqtt.hpp"

#include <ArduinoJson.h>

#include "./wifi.hpp"

namespace lamp {

static constexpr uint32_t MQTT_RECONNECT_INTERVAL_MS = 5000;
static constexpr size_t   MQTT_BUFFER_SIZE = 512;

// PubSubClient's C-style callback can't bind state, so we route through the
// single live instance. Only one MqttComponent is constructed in standard_lamp.
static MqttComponent* s_instance = nullptr;

MqttComponent::MqttComponent() : mqttClient(wifiClient) {}

void MqttComponent::begin(Config* inConfig,
                          std::function<void(uint8_t)> onBrightness,
                          std::function<void(bool)> onPower) {
  config = inConfig;
  brightnessCallback = std::move(onBrightness);
  powerCallback = std::move(onPower);
  s_instance = this;
  mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
  mqttClient.setCallback(messageCallback);
  configureBroker();
}

void MqttComponent::configureBroker() {
  if (!config) return;
  if (!config->mqtt.enabled || config->mqtt.brokerHost.empty()) {
    brokerConfigured = false;
    return;
  }

  lampId = config->lamp.name.empty() ? "lamp" : config->lamp.name;
  std::string prefix = config->mqtt.topicPrefix.empty()
                           ? std::string("homeassistant")
                           : config->mqtt.topicPrefix;

  commandTopic      = prefix + "/light/" + lampId + "/set";
  stateTopic        = prefix + "/light/" + lampId + "/state";
  availabilityTopic = prefix + "/light/" + lampId + "/availability";
  discoveryTopic    = "homeassistant/light/" + lampId + "/config";

  mqttClient.setServer(config->mqtt.brokerHost.c_str(), config->mqtt.brokerPort);
  brokerConfigured = true;

#ifdef LAMP_DEBUG
  Serial.printf("[mqtt] configured: broker=%s:%u prefix=%s lampId=%s\n",
                config->mqtt.brokerHost.c_str(),
                config->mqtt.brokerPort,
                prefix.c_str(),
                lampId.c_str());
#endif
}

void MqttComponent::applyConfig() {
  // Drop any existing connection and re-resolve broker/topics from config.
  if (mqttClient.connected()) {
    publishAvailability(false);
    mqttClient.disconnect();
  }
  configureBroker();
  // Force a fresh attempt soon.
  lastReconnectAttemptMs = 0;
}

void MqttComponent::tick() {
  if (!config || !config->mqtt.enabled) return;
  if (!brokerConfigured) return;
  if (!wifi::isConnected()) {
    // WiFi gone — drop any client state so we reconnect cleanly when it returns.
    if (mqttClient.connected()) mqttClient.disconnect();
    return;
  }

  if (!mqttClient.connected()) {
    uint32_t now = millis();
    if (now - lastReconnectAttemptMs < MQTT_RECONNECT_INTERVAL_MS &&
        lastReconnectAttemptMs != 0) {
      return;
    }
    lastReconnectAttemptMs = now;
    connectBroker();
    return;
  }

  mqttClient.loop();
}

void MqttComponent::connectBroker() {
  std::string clientId = "lamplit-" + lampId;
#ifdef LAMP_DEBUG
  Serial.printf("[mqtt] connecting to broker %s:%u as %s\n",
                config->mqtt.brokerHost.c_str(),
                config->mqtt.brokerPort,
                clientId.c_str());
#endif

  bool ok;
  if (!config->mqtt.username.empty()) {
    ok = mqttClient.connect(clientId.c_str(),
                            config->mqtt.username.c_str(),
                            config->mqtt.password.c_str(),
                            availabilityTopic.c_str(), 0, true, "offline");
  } else {
    ok = mqttClient.connect(clientId.c_str(),
                            nullptr, nullptr,
                            availabilityTopic.c_str(), 0, true, "offline");
  }

  if (!ok) {
#ifdef LAMP_DEBUG
    Serial.printf("[mqtt] connect failed rc=%d\n", mqttClient.state());
#endif
    return;
  }

#ifdef LAMP_DEBUG
  Serial.printf("[mqtt] connected\n");
#endif
  publishDiscovery();
  mqttClient.subscribe(commandTopic.c_str());
  publishAvailability(true);
  publishState();
}

void MqttComponent::publishDiscovery() {
  JsonDocument doc;
  std::string deviceName = config->lamp.name + " Lamp";
  doc["name"] = deviceName;
  doc["unique_id"] = lampId + "_light";
  doc["command_topic"] = commandTopic;
  doc["state_topic"] = stateTopic;
  doc["availability_topic"] = availabilityTopic;
  doc["schema"] = "json";
  doc["brightness"] = true;
  doc["brightness_scale"] = 100;
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";

  JsonObject device = doc["device"].to<JsonObject>();
  device["identifiers"].to<JsonArray>().add(std::string("lamplit_") + lampId);
  device["name"] = deviceName;
  device["manufacturer"] = "Lamplit Art Society";
  device["model"] = "Standard Lamp";

  char buf[MQTT_BUFFER_SIZE];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  bool ok = mqttClient.publish(discoveryTopic.c_str(), buf, true);
#ifdef LAMP_DEBUG
  Serial.printf("[mqtt] discovery %s len=%u\n", ok ? "ok" : "FAIL", (unsigned)len);
#endif
}

void MqttComponent::publishAvailability(bool online) {
  if (!brokerConfigured) return;
  mqttClient.publish(availabilityTopic.c_str(), online ? "online" : "offline", true);
}

void MqttComponent::publishState() {
  if (!brokerConfigured || !mqttClient.connected()) return;

  JsonDocument doc;
  doc["state"] = powerState ? "ON" : "OFF";
  doc["brightness"] = config ? config->lamp.brightness : 0;

  char buf[128];
  serializeJson(doc, buf, sizeof(buf));
  mqttClient.publish(stateTopic.c_str(), buf, true);
#ifdef LAMP_DEBUG
  Serial.printf("[mqtt] state -> %s\n", buf);
#endif
}

void MqttComponent::messageCallback(char* /*topic*/, byte* payload, unsigned int length) {
  if (s_instance) {
    s_instance->handleMessage(reinterpret_cast<const char*>(payload), length);
  }
}

void MqttComponent::handleMessage(const char* payload, unsigned int length) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
#ifdef LAMP_DEBUG
    Serial.printf("[mqtt] cmd parse failed: %s\n", err.c_str());
#endif
    return;
  }
#ifdef LAMP_DEBUG
  Serial.printf("[mqtt] cmd: %.*s\n", (int)length, payload);
#endif

  if (doc["state"].is<const char*>()) {
    const char* s = doc["state"];
    bool on = (strcmp(s, "ON") == 0);
    powerState = on;
    if (powerCallback) powerCallback(on);
  }

  if (doc["brightness"].is<int>()) {
    uint8_t level = doc["brightness"].as<uint8_t>();
    if (level > 100) level = 100;
    if (brightnessCallback) brightnessCallback(level);
  }

  publishState();
}

}  // namespace lamp
