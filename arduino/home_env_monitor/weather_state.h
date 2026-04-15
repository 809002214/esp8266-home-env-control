#ifndef WEATHER_STATE_H
#define WEATHER_STATE_H

#include <Arduino.h>

struct WeatherState {
  // available=true 代表该结构体里已有可用天气数据
  bool available;
  // 温度使用整数，方便在 128x64 上紧凑显示
  int temp;
  int high;
  int low;
  // icon/text 由 MQTT 天气主题更新
  char icon[16];
  char text[24];
};

void initWeatherState(WeatherState& state);
bool parseWeatherPayload(const String& payload, WeatherState& state);

#endif
