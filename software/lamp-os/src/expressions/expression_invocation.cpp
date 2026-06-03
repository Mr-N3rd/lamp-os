#include "./expression_invocation.hpp"

namespace lamp {

std::map<std::string, uint32_t> parametersWithoutCascadeKeys(
    const std::map<std::string, uint32_t>& parameters) {
  std::map<std::string, uint32_t> out;
  for (const auto& kv : parameters) {
    if (kv.first == kParamCascadeEnabled) continue;
    if (kv.first == kParamCascadeStaggerMs) continue;
    out.insert(kv);
  }
  return out;
}

void serializeInvocation(const ExpressionInvocation& inv, std::string& out) {
  JsonDocument doc;
  doc["char"] = "triggerExpression";
  doc["type"] = inv.type;
  doc["target"] = inv.target;
  doc["delayMs"] = inv.delayMs;

  JsonArray colorsArr = doc["colors"].to<JsonArray>();
  for (const Color& c : inv.colors) {
    colorsArr.add(colorToHexString(c));
  }

  JsonObject paramsObj = doc["parameters"].to<JsonObject>();
  for (const auto& kv : inv.parameters) {
    paramsObj[kv.first] = kv.second;
  }

  serializeJson(doc, out);
}

bool parseInvocation(JsonObjectConst doc, ExpressionInvocation& out) {
  const char* type = doc["type"].as<const char*>();
  if (!type || !*type) return false;

  out.type = type;
  out.target = doc["target"] | 3;
  out.delayMs = doc["delayMs"] | 0;

  out.colors.clear();
  JsonArrayConst colors = doc["colors"].as<JsonArrayConst>();
  if (!colors.isNull()) {
    for (JsonVariantConst c : colors) {
      const char* hex = c.as<const char*>();
      if (hex) out.colors.push_back(hexStringToColor(hex));
    }
  }

  out.parameters.clear();
  JsonObjectConst params = doc["parameters"].as<JsonObjectConst>();
  if (!params.isNull()) {
    for (JsonPairConst kv : params) {
      JsonVariantConst v = kv.value();
      if (v.is<uint32_t>()) {
        out.parameters[std::string(kv.key().c_str())] = v.as<uint32_t>();
      } else if (v.is<int>()) {
        out.parameters[std::string(kv.key().c_str())] =
            static_cast<uint32_t>(v.as<int>());
      }
    }
  }

  return true;
}

}  // namespace lamp
