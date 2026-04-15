#include "weather_state.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace {

bool extractJsonString(const String& payload, const char* key, char* out, size_t outSize) {
  const String token = String("\"") + key + "\"";
  const int tokenIndex = payload.indexOf(token);
  if (tokenIndex < 0) {
    return false;
  }

  int colonIndex = payload.indexOf(':', tokenIndex + token.length());
  if (colonIndex < 0) {
    return false;
  }

  int valueStart = colonIndex + 1;
  while (valueStart < payload.length() && isspace(static_cast<unsigned char>(payload[valueStart]))) {
    valueStart++;
  }

  if (valueStart >= payload.length()) {
    return false;
  }

  int valueEnd = valueStart;
  if (payload[valueStart] == '"') {
    valueStart++;
    valueEnd = payload.indexOf('"', valueStart);
    if (valueEnd < 0) {
      return false;
    }
  } else {
    while (valueEnd < payload.length() && payload[valueEnd] != ',' && payload[valueEnd] != '}') {
      valueEnd++;
    }
  }

  String value = payload.substring(valueStart, valueEnd);
  value.trim();
  snprintf(out, outSize, "%s", value.c_str());
  return true;
}

bool extractJsonInt(const String& payload, const char* key, int& outValue) {
  char valueBuffer[16];
  if (!extractJsonString(payload, key, valueBuffer, sizeof(valueBuffer))) {
    return false;
  }

  char* endPointer = nullptr;
  const long parsed = strtol(valueBuffer, &endPointer, 10);
  if (endPointer == valueBuffer) {
    return false;
  }

  outValue = static_cast<int>(parsed);
  return true;
}

bool extractKvString(const String& payload, const char* key, char* out, size_t outSize) {
  const String token = String(key) + "=";
  const int tokenIndex = payload.indexOf(token);
  if (tokenIndex < 0) {
    return false;
  }

  int valueStart = tokenIndex + token.length();
  int valueEnd = payload.indexOf(',', valueStart);
  if (valueEnd < 0) {
    valueEnd = payload.length();
  }

  String value = payload.substring(valueStart, valueEnd);
  value.trim();
  snprintf(out, outSize, "%s", value.c_str());
  return true;
}

bool extractKvInt(const String& payload, const char* key, int& outValue) {
  char valueBuffer[16];
  if (!extractKvString(payload, key, valueBuffer, sizeof(valueBuffer))) {
    return false;
  }

  char* endPointer = nullptr;
  const long parsed = strtol(valueBuffer, &endPointer, 10);
  if (endPointer == valueBuffer) {
    return false;
  }

  outValue = static_cast<int>(parsed);
  return true;
}

void normalizeWeatherText(WeatherState& state) {
  // Adafruit 默认 5x7 字库对中文支持有限，先做轻量映射，避免屏幕乱码。
  if (strstr(state.text, "多云") != nullptr) {
    snprintf(state.text, sizeof(state.text), "DUOYUN");
  } else if (strstr(state.text, "晴") != nullptr) {
    snprintf(state.text, sizeof(state.text), "QING");
  } else if (strstr(state.text, "雨") != nullptr) {
    snprintf(state.text, sizeof(state.text), "YU");
  } else if (strstr(state.text, "雪") != nullptr) {
    snprintf(state.text, sizeof(state.text), "XUE");
  }
}

}  // namespace

void initWeatherState(WeatherState& state) {
  state.available = false;
  state.temp = -1000;
  state.high = -1000;
  state.low = -1000;
  snprintf(state.icon, sizeof(state.icon), "%s", "cloudy");
  snprintf(state.text, sizeof(state.text), "%s", "--");
}

bool parseWeatherPayload(const String& payload, WeatherState& state) {
  bool changed = false;

  if (payload.length() == 0) {
    return false;
  }

  int parsedInt = 0;
  if (extractJsonInt(payload, "temp", parsedInt) || extractKvInt(payload, "temp", parsedInt)) {
    state.temp = parsedInt;
    changed = true;
  }

  if (extractJsonInt(payload, "high", parsedInt) || extractKvInt(payload, "high", parsedInt)) {
    state.high = parsedInt;
    changed = true;
  }

  if (extractJsonInt(payload, "low", parsedInt) || extractKvInt(payload, "low", parsedInt)) {
    state.low = parsedInt;
    changed = true;
  }

  char textBuffer[24];
  if (extractJsonString(payload, "text", textBuffer, sizeof(textBuffer)) ||
      extractKvString(payload, "text", textBuffer, sizeof(textBuffer))) {
    snprintf(state.text, sizeof(state.text), "%s", textBuffer);
    changed = true;
  }

  char iconBuffer[16];
  if (extractJsonString(payload, "icon", iconBuffer, sizeof(iconBuffer)) ||
      extractKvString(payload, "icon", iconBuffer, sizeof(iconBuffer))) {
    snprintf(state.icon, sizeof(state.icon), "%s", iconBuffer);
    changed = true;
  }

  if (changed) {
    state.available = true;
    normalizeWeatherText(state);
  }

  return changed;
}
