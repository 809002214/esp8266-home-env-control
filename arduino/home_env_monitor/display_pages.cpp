#include "display_pages.h"

#include <math.h>
#include <string.h>

namespace {

constexpr uint8_t SCREEN_WIDTH = 128;
constexpr uint8_t TOTAL_PAGES = 8;

String clipText(const String& text, size_t maxChars) {
  if (text.length() <= maxChars) {
    return text;
  }
  if (maxChars < 2) {
    return text.substring(0, maxChars);
  }
  return text.substring(0, maxChars - 1) + "~";
}

String onOff(bool value, const char* onText, const char* offText) {
  return value ? String(onText) : String(offText);
}

String formatFloatValue(float value, uint8_t decimals, const char* fallback = "--") {
  if (isnan(value)) {
    return String(fallback);
  }

  char buffer[16];
  dtostrf(value, 0, decimals, buffer);
  return String(buffer);
}

String formatUptimeCompact(unsigned long uptimeSec) {
  const unsigned long days = uptimeSec / 86400UL;
  const unsigned long hours = (uptimeSec % 86400UL) / 3600UL;
  const unsigned long minutes = (uptimeSec % 3600UL) / 60UL;
  const unsigned long seconds = uptimeSec % 60UL;

  char buffer[28];
  snprintf(buffer, sizeof(buffer), "%luD %02luH %02luM %02luS", days, hours, minutes, seconds);
  return String(buffer);
}

void drawHeader(Adafruit_SSD1306& display, const char* title, uint8_t pageIndex) {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(title);

  char pageBuffer[8];
  snprintf(pageBuffer, sizeof(pageBuffer), "%u/%u", pageIndex + 1, TOTAL_PAGES);

  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  display.getTextBounds(pageBuffer, 0, 0, &x1, &y1, &width, &height);
  display.setCursor(SCREEN_WIDTH - width, 0);
  display.print(pageBuffer);
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
}

void drawKeyValue(Adafruit_SSD1306& display, int y, const char* key, const String& value) {
  display.setTextSize(1);
  display.setCursor(0, y);
  display.print(key);
  display.setCursor(30, y);
  display.print(":");
  display.setCursor(36, y);
  display.print(clipText(value, 15));
}

void drawThermoIcon(Adafruit_SSD1306& display, int x, int y) {
  display.drawLine(x + 3, y, x + 3, y + 10, SSD1306_WHITE);
  display.drawCircle(x + 3, y + 13, 3, SSD1306_WHITE);
}

void drawDropIcon(Adafruit_SSD1306& display, int x, int y) {
  display.drawCircle(x + 4, y + 6, 3, SSD1306_WHITE);
  display.drawLine(x + 4, y, x + 2, y + 4, SSD1306_WHITE);
  display.drawLine(x + 4, y, x + 6, y + 4, SSD1306_WHITE);
}

void drawCloudIcon(Adafruit_SSD1306& display, int x, int y) {
  display.drawCircle(x + 6, y + 8, 4, SSD1306_WHITE);
  display.drawCircle(x + 12, y + 8, 5, SSD1306_WHITE);
  display.drawCircle(x + 18, y + 9, 4, SSD1306_WHITE);
  display.drawLine(x + 4, y + 12, x + 20, y + 12, SSD1306_WHITE);
}

void drawPageEnv(Adafruit_SSD1306& display, const DisplaySnapshot& snapshot) {
  drawHeader(display, "ENV", 0);

  drawThermoIcon(display, 1, 15);
  display.setTextSize(2);
  display.setCursor(18, 14);
  display.print(formatFloatValue(snapshot.temperature, 1));
  display.setTextSize(1);
  display.setCursor(104, 23);
  display.print("C");

  drawDropIcon(display, 0, 39);
  display.setTextSize(2);
  display.setCursor(18, 38);
  display.print(formatFloatValue(snapshot.humidity, 0));
  display.setTextSize(1);
  display.setCursor(104, 47);
  display.print("%");

  display.setCursor(0, 56);
  display.print("SW:");
  display.print(snapshot.switchState ? "ON" : "OFF");
  display.print(" ID:");
  display.print(clipText(snapshot.chipId, 6));
}

void drawPageNet(Adafruit_SSD1306& display, const DisplaySnapshot& snapshot) {
  drawHeader(display, "NET", 1);
  drawKeyValue(display, 14, "WIFI", onOff(snapshot.wifiConnected, "OK", "OFF"));
  drawKeyValue(display, 24, "MQTT", onOff(snapshot.mqttConnected, "OK", "OFF"));
  drawKeyValue(display, 34, "RSSI", snapshot.wifiConnected ? String(snapshot.wifiRssi) + "dBm" : "--");
  drawKeyValue(display, 44, "IP", snapshot.wifiConnected ? snapshot.ip : String("0.0.0.0"));
  drawKeyValue(display, 54, "UP", formatUptimeCompact(snapshot.uptimeSec));
}

void drawPageClock(Adafruit_SSD1306& display, const DisplaySnapshot& snapshot) {
  drawHeader(display, "TIME", 2);
  display.setTextSize(2);
  display.setCursor(2, 14);
  display.print(snapshot.localTimeShort);
  display.setTextSize(1);
  drawKeyValue(display, 36, "DATE", snapshot.localDate);
  drawKeyValue(display, 46, "NTP", onOff(snapshot.timeSynced, "SYNC", "WAIT"));
  drawKeyValue(display, 54, "UP", formatUptimeZh(snapshot.uptimeSec));
}

void drawPageDevice(Adafruit_SSD1306& display, const DisplaySnapshot& snapshot) {
  drawHeader(display, "DEV", 3);
  drawKeyValue(display, 14, "NAME", snapshot.friendlyName);
  drawKeyValue(display, 24, "MODEL", "ESP8266");
  drawKeyValue(display, 34, "FW", snapshot.firmwareVersion);
  drawKeyValue(display, 44, "SN", "SN-" + snapshot.chipId);
  drawKeyValue(display, 54, "ID", snapshot.chipId);
}

void drawPageCalibration(Adafruit_SSD1306& display, const DisplaySnapshot& snapshot) {
  drawHeader(display, "CALI", 4);
  drawKeyValue(display, 14, "TZ", String(snapshot.timezoneHours, 1));
  drawKeyValue(display, 24, "T-OFF", formatFloatValue(snapshot.tempOffset, 1));
  drawKeyValue(display, 34, "H-OFF", formatFloatValue(snapshot.humidityOffset, 1));
  drawKeyValue(display, 44, "CFG", onOff(snapshot.configValid, "OK", "ERR"));
  drawKeyValue(display, 54, "PAGE", String(snapshot.pageIntervalSec) + "s");
}

void drawPageWeather(Adafruit_SSD1306& display, const DisplaySnapshot& snapshot) {
  drawHeader(display, "WEA", 5);

  drawCloudIcon(display, 2, 14);

  display.setTextSize(2);
  display.setCursor(38, 14);
  if (snapshot.weather.available && snapshot.weather.temp > -100) {
    display.print(snapshot.weather.temp);
  } else {
    display.print("--");
  }

  display.setTextSize(1);
  display.setCursor(88, 22);
  display.print("C");

  drawKeyValue(
      display,
      34,
      "HIGH",
      snapshot.weather.available && snapshot.weather.high > -100 ? String(snapshot.weather.high) : String("--"));
  drawKeyValue(
      display,
      44,
      "LOW",
      snapshot.weather.available && snapshot.weather.low > -100 ? String(snapshot.weather.low) : String("--"));
  drawKeyValue(display, 54, "TXT", snapshot.weather.available ? String(snapshot.weather.text) : String("--"));
}

void drawPageOta(Adafruit_SSD1306& display, const DisplaySnapshot& snapshot) {
  drawHeader(display, "OTA", 6);
  drawKeyValue(display, 14, "STAT", snapshot.otaState);
  drawKeyValue(display, 24, "PORT", String(snapshot.otaPort));
  drawKeyValue(display, 34, "PATH", snapshot.otaPath);
  drawKeyValue(display, 44, "MODE", snapshot.otaInProgress ? "RUNNING" : "IDLE");
  drawKeyValue(display, 54, "CTRL", "MQTT/WEB");
}

void drawPageFault(Adafruit_SSD1306& display, const DisplaySnapshot& snapshot) {
  drawHeader(display, "FAULT", 7);
  drawKeyValue(display, 14, "ERR", snapshot.lastError);
  drawKeyValue(display, 24, "DS18", onOff(snapshot.ds18b20Ok, "OK", "ERR"));
  drawKeyValue(display, 34, "DHT11", onOff(snapshot.dht11Ok, "OK", "ERR"));
  drawKeyValue(display, 44, "CFG", onOff(snapshot.configValid, "OK", "ERR"));
  drawKeyValue(display, 54, "AUTO", "RETRY");
}

}  // namespace

String formatUptimeZh(unsigned long uptimeSec) {
  const unsigned long days = uptimeSec / 86400UL;
  const unsigned long hours = (uptimeSec % 86400UL) / 3600UL;
  const unsigned long minutes = (uptimeSec % 3600UL) / 60UL;
  const unsigned long seconds = uptimeSec % 60UL;

  char buffer[40];
  snprintf(buffer, sizeof(buffer), "%lu:%02lu:%02lu:%02lu", days, hours, minutes, seconds);
  return String(buffer);
}

void renderDisplayPages(
    Adafruit_SSD1306& display,
    const DisplaySnapshot& snapshot,
    unsigned long nowMs,
    unsigned long pageIntervalMs) {
  const uint8_t pageIndex = static_cast<uint8_t>((nowMs / pageIntervalMs) % TOTAL_PAGES);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  switch (pageIndex) {
    case 0:
      drawPageEnv(display, snapshot);
      break;
    case 1:
      drawPageNet(display, snapshot);
      break;
    case 2:
      drawPageClock(display, snapshot);
      break;
    case 3:
      drawPageDevice(display, snapshot);
      break;
    case 4:
      drawPageCalibration(display, snapshot);
      break;
    case 5:
      drawPageWeather(display, snapshot);
      break;
    case 6:
      drawPageOta(display, snapshot);
      break;
    default:
      drawPageFault(display, snapshot);
      break;
  }

  display.display();
}
