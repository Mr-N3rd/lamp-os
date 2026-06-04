#include "WispOpDispatcher.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "WispConfig.h"

namespace wisp {

DispatchResult WispOpDispatcher::dispatch(const uint8_t* payload, size_t len) {
  if (!payload || len == 0) {
    return DispatchResult::Malformed;
  }

  // ArduinoJson v7: unified JsonDocument. Aurora-stack budget is comfortable
  // with a small doc for these short ops (well under 256 bytes typical).
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, len);
  if (err) {
    Serial.printf("[wisp.op] malformed JSON: %s\n", err.c_str());
    return DispatchResult::Malformed;
  }

  const char* charField = doc["char"];
  if (!charField) {
    // No "char" field at all — could be a different envelope shape. Don't
    // warn loudly; this is the same "not for us" bucket as a foreign char.
    return DispatchResult::Ignored;
  }

  // Anything other than "wispOp" is something we don't speak. wispStatus
  // gossip-relay echoes land here and get silently dropped, per spec.
  if (strcmp(charField, "wispOp") != 0) {
    return DispatchResult::Ignored;
  }

  const char* op = doc["op"];
  if (!op) {
    Serial.println("[wisp.op] wispOp missing 'op' field");
    return DispatchResult::Malformed;
  }

  if (strcmp(op, "setZone") == 0) {
    // zoneId is required and must be an int. ArduinoJson's is<int>() check
    // covers numeric type compatibility (rejects strings/floats/missing).
    if (!doc["zoneId"].is<int>()) {
      Serial.println("[wisp.op] setZone missing/invalid 'zoneId'");
      return DispatchResult::Malformed;
    }
    const int zoneId = doc["zoneId"].as<int>();
    // Negative-zoneId rejection lives in WispConfig::setSelectedZone — the
    // storage layer owns that invariant, so the dispatcher doesn't duplicate
    // the check here.
    Serial.printf("[wisp.op] setZone zoneId=%d\n", zoneId);
    config_.setSelectedZone(zoneId);
    return DispatchResult::AppliedZoneChange;
  }

  if (strcmp(op, "clearZone") == 0) {
    Serial.println("[wisp.op] clearZone");
    config_.clearSelectedZone();
    return DispatchResult::AppliedZoneChange;
  }

  if (strcmp(op, "setWifi") == 0) {
    const char* ssid = doc["ssid"];
    const char* pw   = doc["pw"];
    if (!ssid || !pw) {
      Serial.println("[wisp.op] setWifi missing 'ssid' or 'pw'");
      return DispatchResult::Malformed;
    }
    // Stub: store via WispConfig but don't act on it. STA bring-up wiring
    // lands in a later phase.
    Serial.printf("[wisp.op] setWifi ssid='%s' pw=<%u chars> (stub stored)\n",
                  ssid, (unsigned)strlen(pw));
    config_.setWifi(String(ssid), String(pw));
    return DispatchResult::AppliedWifiChange;
  }

  Serial.printf("[wisp.op] unknown wispOp op='%s'\n", op);
  return DispatchResult::Malformed;
}

}  // namespace wisp
