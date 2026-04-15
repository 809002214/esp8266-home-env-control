#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "display_pages.h"
#include "weather_state.h"

// ---------------------------
// 产品基础信息
// 这几项相当于“设备身份证”
// ---------------------------
const char* DEVICE_ID = "home-room1-env01";
const char* DEVICE_MODEL = "ESP8266_ENV_MONITOR";
const char* HARDWARE_REVISION = "NODEMCU_V3";
const char* FIRMWARE_VERSION = "1.3.0";
const char* CONFIG_AP_PASSWORD = "12345678";

// ---------------------------
// MQTT 主题
// 现有网页与 Home Assistant 可以继续复用原来的温湿度和开关主题
// 新增 device/info、system/state、system/event、system/reset 这些产品化主题
// ---------------------------
const char* TOPIC_TEMPERATURE = "home/room1/env01/temperature";
const char* TOPIC_HUMIDITY = "home/room1/env01/humidity";
const char* TOPIC_STATUS = "home/room1/env01/status";
const char* TOPIC_SWITCH_STATE = "home/room1/env01/switch/state";
const char* TOPIC_SWITCH_SET = "home/room1/env01/switch/set";
const char* TOPIC_DEVICE_INFO = "home/room1/env01/device/info";
const char* TOPIC_SYSTEM_STATE = "home/room1/env01/system/state";
const char* TOPIC_SYSTEM_EVENT = "home/room1/env01/system/event";
const char* TOPIC_SYSTEM_RESET = "home/room1/env01/system/reset";
const char* TOPIC_OTA_SET = "home/room1/env01/ota/set";
const char* TOPIC_OTA_STATE = "home/room1/env01/ota/state";
const char* TOPIC_WEATHER_STATE = "home/room1/env01/weather/state";
const char* NTP_SERVER_PRIMARY = "ntp.aliyun.com";
const char* NTP_SERVER_SECONDARY = "pool.ntp.org";

// ---------------------------
// 引脚定义
// D0: OLED RES
// D1: OLED D0(SCLK)
// D2: OLED D1(MOSI)
// D3: OLED DC
// D4: OLED CS
// D5: DS18B20 温度传感器
// D6: DHT11 湿度传感器
// D7: 预留给继电器/开关控制
// ---------------------------
constexpr uint8_t PIN_OLED_RESET = D0;
constexpr uint8_t PIN_OLED_SCLK = D1;
constexpr uint8_t PIN_OLED_MOSI = D2;
constexpr uint8_t PIN_OLED_DC = D3;
constexpr uint8_t PIN_OLED_CS = D4;
constexpr uint8_t PIN_ONEWIRE = D5;
constexpr uint8_t PIN_DHT = D6;
constexpr uint8_t PIN_SWITCH = D7;
constexpr uint8_t DHT_TYPE = DHT11;

// OLED 参数
// 你这块 7 针屏是 SPI 版，不是 I2C 版
constexpr uint8_t SCREEN_WIDTH = 128;
constexpr uint8_t SCREEN_HEIGHT = 64;

// 这里控制开关脚是高电平有效还是低电平有效
// 如果以后接的继电器模块是“低电平触发”，把它改成 false
constexpr bool SWITCH_ACTIVE_HIGH = true;

// 定时参数
constexpr unsigned long SENSOR_INTERVAL_MS = 5000;
constexpr unsigned long DISPLAY_INTERVAL_MS = 1000;
constexpr unsigned long DISPLAY_PAGE_INTERVAL_MS = 8000;
constexpr unsigned long SYSTEM_STATE_INTERVAL_MS = 15000;
constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr unsigned long MQTT_RETRY_MS = 5000;
constexpr unsigned long NTP_CONFIG_INTERVAL_MS = 60000;
constexpr unsigned long DISPLAY_RECOVERY_MS = 30000;
constexpr unsigned long WIFI_RECOVERY_PORTAL_MS = 300000;
constexpr unsigned long MQTT_RECOVERY_PORTAL_MS = 300000;
constexpr unsigned long RESET_ARM_WINDOW_MS = 60000;
constexpr uint32_t CONFIG_MAGIC = 0x484F4D48;
constexpr size_t EEPROM_SIZE = 896;
constexpr uint8_t PIN_BOARD_LED = LED_BUILTIN;

// MQTT 参数和设备名会保存在 Flash 里，断电后仍然保留
struct PersistentConfig {
  uint32_t magic;
  char friendlyName[32];
  char mqttHost[40];
  char mqttPort[6];
  char mqttUsername[32];
  char mqttPassword[32];
  char temperatureOffset[8];
  char humidityOffset[8];
  char timezoneHours[8];
  char otaPort[6];
  char otaPath[96];
  // 夜间模式起止小时（0-23），用于自动熄屏和关板载灯
  char nightStartHour[3];
  char nightEndHour[3];
  // 远程恢复出厂开关：0=禁用（默认），1=启用
  char remoteFactoryResetEnabled[2];
};

// 硬件与通信对象初始化
// 这里使用软件 SPI，正好对应你当前 OLED 的 7 针排针定义
Adafruit_SSD1306 display(
    SCREEN_WIDTH,
    SCREEN_HEIGHT,
    PIN_OLED_MOSI,
    PIN_OLED_SCLK,
    PIN_OLED_DC,
    PIN_OLED_RESET,
    PIN_OLED_CS);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature ds18b20(&oneWire);
DHT dht(PIN_DHT, DHT_TYPE);

// 采样结果缓存
float temperatureDs18b20 = NAN;
float humidityDht11 = NAN;
float temperatureDht11 = NAN;
bool switchState = false;
bool displayReady = false;
bool shouldSaveConfig = false;
bool configValid = false;
bool ds18b20Ok = false;
bool dht11Ok = false;
bool timeSynced = false;
bool otaInProgress = false;
bool arduinoOtaReady = false;
uint8_t lastOtaProgressPercent = 255;
// displayPanelOn: 面板电源状态（DISPLAYON/OFF）
// nightModeActive: 当前是否处于夜间模式
// remoteResetArmUntilMs: 远程恢复出厂的二次确认窗口截止时间
bool displayPanelOn = true;
bool nightModeActive = false;
unsigned long remoteResetArmUntilMs = 0;

PersistentConfig appConfig = {
    CONFIG_MAGIC,
    "",
    "",
    "1883",
    "",
    "",
    "0.0",
    "0.0",
    "8.0",
    "1880",
    "/firmware/esp8266-home-env-control.bin",
    "23",
    "7",
    "0",
};

// 设备身份相关的运行时信息
String deviceSerialNumber;
String runtimeDeviceId;
String bootResetReason;

// 状态与故障信息
char lastErrorText[64] = "BOOTING";
char lastConfigErrorText[96] = "";
char otaStateText[96] = "IDLE";
WeatherState weatherState;

// 定时器时间戳
unsigned long lastSensorReadMs = 0;
unsigned long lastDisplayMs = 0;
unsigned long lastSystemStateMs = 0;
unsigned long lastWifiRetryMs = 0;
unsigned long lastMqttRetryMs = 0;
unsigned long lastNtpConfigMs = 0;
unsigned long lastDisplayRecoveryMs = 0;
unsigned long wifiDisconnectSinceMs = 0;
unsigned long mqttDisconnectSinceMs = 0;

void publishOtaState();
void setupArduinoOtaIfNeeded();
bool performHttpOtaUpdate();
bool performHttpOtaUpdate(const char* customPath);
bool isAllDigits(const char* value);
void setupRuntimeIdentity();
void resetAppConfigToDefaults();
void loadConfigFromEeprom();
bool validateCurrentConfig(char* reasonBuffer, size_t reasonBufferSize);
void setLastConfigError(const char* errorText);
void refreshErrorSummary();
void setBoardLed(bool on);
void applySwitchState(bool on);
void setupDisplay();
void readSensors();
bool isTimeAvailable();
String getFriendlyName();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void applyMqttServerConfig();
bool configureWiFiWithPortal(bool forcePortal);
void configureNtpIfNeeded(bool forceUpdate);
void connectWiFiIfNeeded();
void connectMqttIfNeeded();
void handleRecoveryPolicies();
void updateNightModeIfNeeded();
void publishSystemEvent(const String& eventText);
void publishSystemState();
void publishSensorValues();
void printSerialStatus();
void renderDisplay();

// 其余服务函数已拆分到 home_env_monitor_services.ino

void setup() {
  Serial.begin(115200);
  delay(100);

  setupRuntimeIdentity();
  initWeatherState(weatherState);
  resetAppConfigToDefaults();
  loadConfigFromEeprom();

  char validationReason[96];
  configValid = validateCurrentConfig(validationReason, sizeof(validationReason));
  setLastConfigError(configValid ? "" : validationReason);
  refreshErrorSummary();

  pinMode(PIN_SWITCH, OUTPUT);
  pinMode(PIN_BOARD_LED, OUTPUT);
  setBoardLed(true);
  applySwitchState(false);

  setupDisplay();

  ds18b20.begin();
  dht.begin();
  readSensors();
  timeSynced = isTimeAvailable();

  WiFi.mode(WIFI_STA);
  const String friendlyName = getFriendlyName();
  WiFi.hostname(friendlyName.c_str());

  mqttClient.setBufferSize(1024);
  mqttClient.setCallback(mqttCallback);
  applyMqttServerConfig();

  // 启动时优先走自动连接；如果没有配置、配置无效或 Wi-Fi 连不上，就进入手机配网页
  const bool forcePortal = !configValid;
  configureWiFiWithPortal(forcePortal);

  char postConfigReason[96];
  configValid = validateCurrentConfig(postConfigReason, sizeof(postConfigReason));
  setLastConfigError(configValid ? "" : postConfigReason);
  const String configuredName = getFriendlyName();
  WiFi.hostname(configuredName.c_str());
  configureNtpIfNeeded(true);
  applyMqttServerConfig();
  setupArduinoOtaIfNeeded();
  refreshErrorSummary();
}

void loop() {
  connectWiFiIfNeeded();
  setupArduinoOtaIfNeeded();
  connectMqttIfNeeded();

  if (mqttClient.connected()) {
    mqttClient.loop();
  }

  if (arduinoOtaReady && WiFi.isConnected()) {
    ArduinoOTA.handle();
  }

  handleRecoveryPolicies();

  const bool previousTimeSynced = timeSynced;
  timeSynced = isTimeAvailable();
  if (!previousTimeSynced && timeSynced && mqttClient.connected()) {
    publishSystemEvent("NTP_TIME_SYNCED");
    publishSystemState();
  }

  updateNightModeIfNeeded();

  const unsigned long now = millis();

  if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    lastSensorReadMs = now;
    readSensors();
    refreshErrorSummary();

    if (mqttClient.connected()) {
      publishSensorValues();
      publishSystemState();
    }

    printSerialStatus();
  }

  if (now - lastSystemStateMs >= SYSTEM_STATE_INTERVAL_MS) {
    lastSystemStateMs = now;
    refreshErrorSummary();

    if (mqttClient.connected()) {
      publishSystemState();
    }
  }

  if (now - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
    lastDisplayMs = now;
    renderDisplay();
  }
}
