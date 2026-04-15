#ifndef DISPLAY_PAGES_H
#define DISPLAY_PAGES_H

#include <Adafruit_SSD1306.h>
#include <Arduino.h>

#include "weather_state.h"

struct DisplaySnapshot {
  // 基础信息
  String friendlyName;
  String chipId;
  String firmwareVersion;
  String ip;
  String localTimeShort;
  String localDate;
  String lastError;
  String otaState;
  String otaPath;
  // 连接与状态位
  bool wifiConnected;
  bool mqttConnected;
  bool switchState;
  bool configValid;
  bool ds18b20Ok;
  bool dht11Ok;
  bool timeSynced;
  bool otaInProgress;
  // 传感器与参数
  float temperature;
  float humidity;
  float tempOffset;
  float humidityOffset;
  float timezoneHours;
  long wifiRssi;
  unsigned long uptimeSec;
  uint8_t pageIntervalSec;
  uint16_t otaPort;
  // 天气缓存
  WeatherState weather;
};

String formatUptimeZh(unsigned long uptimeSec);
void renderDisplayPages(
    Adafruit_SSD1306& display,
    const DisplaySnapshot& snapshot,
    unsigned long nowMs,
    unsigned long pageIntervalMs);

#endif
