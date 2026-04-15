#include <ESP8266WiFi.h>
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

// ---------------------------
// 产品基础信息
// 这几项相当于“设备身份证”
// ---------------------------
const char* DEVICE_ID = "home-room1-env01";
const char* DEVICE_MODEL = "ESP8266_ENV_MONITOR";
const char* HARDWARE_REVISION = "NODEMCU_V3";
const char* FIRMWARE_VERSION = "1.2.0";
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
constexpr unsigned long DISPLAY_PAGE_INTERVAL_MS = 5000;
constexpr unsigned long SYSTEM_STATE_INTERVAL_MS = 15000;
constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr unsigned long MQTT_RETRY_MS = 5000;
constexpr unsigned long NTP_CONFIG_INTERVAL_MS = 60000;
constexpr unsigned long DISPLAY_RECOVERY_MS = 30000;
constexpr unsigned long WIFI_RECOVERY_PORTAL_MS = 300000;
constexpr unsigned long MQTT_RECOVERY_PORTAL_MS = 300000;
constexpr uint32_t CONFIG_MAGIC = 0x484F4D48;
constexpr size_t EEPROM_SIZE = 896;

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
};

// 设备身份相关的运行时信息
String deviceSerialNumber;
String runtimeDeviceId;
String bootResetReason;

// 状态与故障信息
char lastErrorText[64] = "BOOTING";
char lastConfigErrorText[96] = "";

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

// 把本机 IP 转成字符串，方便显示和上报
String ipToString(IPAddress ip) {
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}

// 把芯片 ID 转成固定长度十六进制字符串
String getChipHexId() {
  char buffer[9];
  snprintf(buffer, sizeof(buffer), "%06X", ESP.getChipId());
  return String(buffer);
}

String getDefaultFriendlyName() {
  return String("EnvMonitor-") + getChipHexId();
}

String getFriendlyName() {
  if (appConfig.friendlyName[0] != '\0') {
    return String(appConfig.friendlyName);
  }
  return getDefaultFriendlyName();
}

bool parseFloatValue(const char* value, float& output) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }

  char* endPointer = nullptr;
  output = strtof(value, &endPointer);
  return endPointer != value && *endPointer == '\0';
}

float getTemperatureOffset() {
  float value = 0.0f;
  if (parseFloatValue(appConfig.temperatureOffset, value)) {
    return value;
  }
  return 0.0f;
}

float getHumidityOffset() {
  float value = 0.0f;
  if (parseFloatValue(appConfig.humidityOffset, value)) {
    return value;
  }
  return 0.0f;
}

float getTimezoneHours() {
  float value = 8.0f;
  if (parseFloatValue(appConfig.timezoneHours, value)) {
    return value;
  }
  return 8.0f;
}

bool isTimeAvailable() {
  time_t now = time(nullptr);
  return now >= 1700000000;
}

String formatLocalTimeString() {
  if (!isTimeAvailable()) {
    return String("Time syncing");
  }

  time_t now = time(nullptr);
  struct tm timeInfo;
  localtime_r(&now, &timeInfo);

  char buffer[24];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeInfo);
  return String(buffer);
}

String formatLocalTimeShort() {
  if (!isTimeAvailable()) {
    return String("--:--:--");
  }

  time_t now = time(nullptr);
  struct tm timeInfo;
  localtime_r(&now, &timeInfo);

  char buffer[16];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeInfo);
  return String(buffer);
}

void configureNtpIfNeeded(bool forceUpdate = false) {
  if (!WiFi.isConnected()) {
    return;
  }

  const unsigned long nowMs = millis();
  if (!forceUpdate && nowMs - lastNtpConfigMs < NTP_CONFIG_INTERVAL_MS) {
    return;
  }

  lastNtpConfigMs = nowMs;
  const long timezoneOffsetSeconds = static_cast<long>(getTimezoneHours() * 3600.0f);
  configTime(timezoneOffsetSeconds, 0, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);
}

// JSON 里需要避免双引号和换行破坏格式
String sanitizeForJson(const String& input) {
  String output = input;
  output.replace("\\", "/");
  output.replace("\"", "'");
  output.replace("\r", " ");
  output.replace("\n", " ");
  return output;
}

// 安全复制字符串，避免数组越界
void copyString(char* target, size_t targetSize, const char* source) {
  if (targetSize == 0) {
    return;
  }

  if (source == nullptr) {
    target[0] = '\0';
    return;
  }

  snprintf(target, targetSize, "%s", source);
}

void setLastError(const char* errorText) {
  copyString(lastErrorText, sizeof(lastErrorText), errorText);
}

void setLastConfigError(const char* errorText) {
  copyString(lastConfigErrorText, sizeof(lastConfigErrorText), errorText);
}

// 读取保存过的配置
void loadConfigFromEeprom() {
  EEPROM.begin(EEPROM_SIZE);

  PersistentConfig storedConfig;
  EEPROM.get(0, storedConfig);

  if (storedConfig.magic == CONFIG_MAGIC) {
    appConfig = storedConfig;
  }

  EEPROM.end();

  appConfig.friendlyName[sizeof(appConfig.friendlyName) - 1] = '\0';
  appConfig.mqttHost[sizeof(appConfig.mqttHost) - 1] = '\0';
  appConfig.mqttPort[sizeof(appConfig.mqttPort) - 1] = '\0';
  appConfig.mqttUsername[sizeof(appConfig.mqttUsername) - 1] = '\0';
  appConfig.mqttPassword[sizeof(appConfig.mqttPassword) - 1] = '\0';
  appConfig.temperatureOffset[sizeof(appConfig.temperatureOffset) - 1] = '\0';
  appConfig.humidityOffset[sizeof(appConfig.humidityOffset) - 1] = '\0';
  appConfig.timezoneHours[sizeof(appConfig.timezoneHours) - 1] = '\0';
}

// 把配置保存到 Flash
// 注意：Flash 有擦写寿命，所以只在配置变化时保存
bool saveConfigToEeprom() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, appConfig);
  const bool committed = EEPROM.commit();
  EEPROM.end();
  return committed;
}

void resetAppConfigToDefaults() {
  memset(&appConfig, 0, sizeof(appConfig));
  appConfig.magic = CONFIG_MAGIC;
  copyString(appConfig.friendlyName, sizeof(appConfig.friendlyName), getDefaultFriendlyName().c_str());
  copyString(appConfig.mqttPort, sizeof(appConfig.mqttPort), "1883");
  copyString(appConfig.temperatureOffset, sizeof(appConfig.temperatureOffset), "0.0");
  copyString(appConfig.humidityOffset, sizeof(appConfig.humidityOffset), "0.0");
  copyString(appConfig.timezoneHours, sizeof(appConfig.timezoneHours), "8.0");
}

bool isAllDigits(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }

  for (size_t i = 0; value[i] != '\0'; i++) {
    if (value[i] < '0' || value[i] > '9') {
      return false;
    }
  }

  return true;
}

// 校验配置
// Wi-Fi 账号密码由 WiFiManager 自己处理
bool validateCurrentConfig(char* reasonBuffer, size_t reasonBufferSize) {
  if (strlen(appConfig.friendlyName) == 0) {
    copyString(reasonBuffer, reasonBufferSize, "设备名不能为空");
    return false;
  }

  if (strlen(appConfig.mqttHost) == 0) {
    copyString(reasonBuffer, reasonBufferSize, "MQTT服务器地址不能为空");
    return false;
  }

  if (!isAllDigits(appConfig.mqttPort)) {
    copyString(reasonBuffer, reasonBufferSize, "MQTT端口必须是数字");
    return false;
  }

  const long port = strtol(appConfig.mqttPort, nullptr, 10);
  if (port <= 0 || port > 65535) {
    copyString(reasonBuffer, reasonBufferSize, "MQTT端口范围必须在1到65535");
    return false;
  }

  float temperatureOffset = 0.0f;
  if (!parseFloatValue(appConfig.temperatureOffset, temperatureOffset)) {
    copyString(reasonBuffer, reasonBufferSize, "温度校准值格式不正确");
    return false;
  }

  if (temperatureOffset < -20.0f || temperatureOffset > 20.0f) {
    copyString(reasonBuffer, reasonBufferSize, "温度校准值建议在-20到20之间");
    return false;
  }

  float humidityOffset = 0.0f;
  if (!parseFloatValue(appConfig.humidityOffset, humidityOffset)) {
    copyString(reasonBuffer, reasonBufferSize, "湿度校准值格式不正确");
    return false;
  }

  if (humidityOffset < -50.0f || humidityOffset > 50.0f) {
    copyString(reasonBuffer, reasonBufferSize, "湿度校准值建议在-50到50之间");
    return false;
  }

  float timezoneHours = 8.0f;
  if (!parseFloatValue(appConfig.timezoneHours, timezoneHours)) {
    copyString(reasonBuffer, reasonBufferSize, "时区小时格式不正确");
    return false;
  }

  if (timezoneHours < -12.0f || timezoneHours > 14.0f) {
    copyString(reasonBuffer, reasonBufferSize, "时区小时建议在-12到14之间");
    return false;
  }

  reasonBuffer[0] = '\0';
  return true;
}

uint16_t getMqttPort() {
  long port = strtol(appConfig.mqttPort, nullptr, 10);
  if (port <= 0 || port > 65535) {
    return 1883;
  }
  return static_cast<uint16_t>(port);
}

bool isMqttConfigured() {
  return configValid && appConfig.mqttHost[0] != '\0';
}

void applyMqttServerConfig() {
  if (!isMqttConfigured()) {
    return;
  }

  mqttClient.setServer(appConfig.mqttHost, getMqttPort());
}

void saveConfigCallback() {
  shouldSaveConfig = true;
}

void showDisplayMessage(const String& line1,
                        const String& line2 = "",
                        const String& line3 = "",
                        const String& line4 = "") {
  if (!displayReady) {
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(line1);
  display.println(line2);
  display.println(line3);
  display.println(line4);
  display.display();
}

void publishSystemEvent(const String& eventText) {
  if (!mqttClient.connected()) {
    return;
  }

  const String safeEventText = sanitizeForJson(eventText);
  char payload[256];
  snprintf(payload,
           sizeof(payload),
           "{\"device_id\":\"%s\",\"serial_number\":\"%s\",\"event\":\"%s\",\"uptime_sec\":%lu}",
           runtimeDeviceId.c_str(),
           deviceSerialNumber.c_str(),
           safeEventText.c_str(),
           millis() / 1000UL);

  mqttClient.publish(TOPIC_SYSTEM_EVENT, payload, false);
}

void factoryResetAndRestart(const char* reasonText) {
  setLastError(reasonText);
  showDisplayMessage("Factory Reset", getFriendlyName(), "Clear WiFi + MQTT", "Restarting...");
  Serial.print("恢复出厂设置: ");
  Serial.println(reasonText);

  WiFiManager wifiManager;
  wifiManager.resetSettings();
  resetAppConfigToDefaults();
  saveConfigToEeprom();

  delay(1500);
  ESP.restart();
}

void enterRecoveryConfigPortal(const char* reasonText) {
  setLastError(reasonText);
  showDisplayMessage("Recovery Portal", getFriendlyName(), reasonText, "Open 192.168.4.1");
  Serial.print("进入恢复配网: ");
  Serial.println(reasonText);

  mqttClient.disconnect();
  configureWiFiWithPortal(true);

  char validationReason[96];
  configValid = validateCurrentConfig(validationReason, sizeof(validationReason));
  setLastConfigError(configValid ? "" : validationReason);

  const String configuredName = getFriendlyName();
  WiFi.hostname(configuredName.c_str());
  applyMqttServerConfig();
  configureNtpIfNeeded(true);

  wifiDisconnectSinceMs = 0;
  mqttDisconnectSinceMs = 0;
  lastWifiRetryMs = 0;
  lastMqttRetryMs = 0;
  timeSynced = isTimeAvailable();
  refreshErrorSummary();
}

void restartDevice(const char* reasonText) {
  setLastError(reasonText);
  showDisplayMessage("System Restart", getFriendlyName(), reasonText, "Please wait...");
  Serial.print("设备重启: ");
  Serial.println(reasonText);
  delay(1200);
  ESP.restart();
}

// 进入配网模式时，在串口和 OLED 上提示当前热点信息
void configModeCallback(WiFiManager* wifiManager) {
  Serial.println("进入配网模式");
  Serial.print("热点名称: ");
  Serial.println(wifiManager->getConfigPortalSSID());
  Serial.print("配网页面: ");
  Serial.println(WiFi.softAPIP());

  showDisplayMessage("Config Mode",
                     wifiManager->getConfigPortalSSID(),
                     "Open 192.168.4.1",
                     getFriendlyName());
}

// 配网页面顶部插入一段说明，让用户更清楚自己在配置哪台设备
String buildPortalInfoHtml() {
  String html;
  html += "<div style='padding:10px;border:1px solid #ddd;border-radius:8px;margin:10px 0;'>";
  html += "<strong>设备名：</strong>" + sanitizeForJson(getFriendlyName()) + "<br>";
  html += "<strong>型号：</strong>" + String(DEVICE_MODEL) + "<br>";
  html += "<strong>序列号：</strong>" + deviceSerialNumber + "<br>";
  html += "<strong>固件版本：</strong>" + String(FIRMWARE_VERSION) + "<br>";
  html += "<span>Wi-Fi列表里会显示信号强度，请优先选择信号更好的路由器。</span><br>";
  html += "<span>温度校准和湿度校准支持负数，例如 -0.5。</span><br>";
  html += "<span>时区小时例如中国是 8.0，支持小数时区。</span><br>";
  html += "<span>如需恢复出厂，请在“恢复出厂确认”里输入 RESET。</span>";
  html += "</div>";
  return html;
}

// 手机配网入口
// 如果有保存的 Wi-Fi，会优先自动连接
// 如果没有，或连接失败，就开启热点让手机配置
bool configureWiFiWithPortal(bool forcePortal) {
  while (true) {
    shouldSaveConfig = false;
    PersistentConfig previousConfig = appConfig;

    WiFiManager wifiManager;
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    wifiManager.setAPCallback(configModeCallback);
    wifiManager.setConfigPortalTimeout(180);

    const String portalInfoHtml = buildPortalInfoHtml();
    char friendlyNameBuffer[sizeof(appConfig.friendlyName)];
    char mqttHostBuffer[sizeof(appConfig.mqttHost)];
    char mqttPortBuffer[sizeof(appConfig.mqttPort)];
    char mqttUsernameBuffer[sizeof(appConfig.mqttUsername)];
    char mqttPasswordBuffer[sizeof(appConfig.mqttPassword)];
    char temperatureOffsetBuffer[sizeof(appConfig.temperatureOffset)];
    char humidityOffsetBuffer[sizeof(appConfig.humidityOffset)];
    char timezoneHoursBuffer[sizeof(appConfig.timezoneHours)];
    char factoryResetBuffer[8] = "";

    copyString(friendlyNameBuffer, sizeof(friendlyNameBuffer), getFriendlyName().c_str());
    copyString(mqttHostBuffer, sizeof(mqttHostBuffer), appConfig.mqttHost);
    copyString(mqttPortBuffer, sizeof(mqttPortBuffer), appConfig.mqttPort);
    copyString(mqttUsernameBuffer, sizeof(mqttUsernameBuffer), appConfig.mqttUsername);
    copyString(mqttPasswordBuffer, sizeof(mqttPasswordBuffer), appConfig.mqttPassword);
    copyString(temperatureOffsetBuffer, sizeof(temperatureOffsetBuffer), appConfig.temperatureOffset);
    copyString(humidityOffsetBuffer, sizeof(humidityOffsetBuffer), appConfig.humidityOffset);
    copyString(timezoneHoursBuffer, sizeof(timezoneHoursBuffer), appConfig.timezoneHours);

    WiFiManagerParameter customPortalInfo(portalInfoHtml.c_str());
    WiFiManagerParameter customFriendlyName("friendly_name", "设备名", friendlyNameBuffer, sizeof(friendlyNameBuffer));
    WiFiManagerParameter customMqttHost("mqtt_host", "MQTT服务器地址", mqttHostBuffer, sizeof(mqttHostBuffer));
    WiFiManagerParameter customMqttPort("mqtt_port", "MQTT端口", mqttPortBuffer, sizeof(mqttPortBuffer));
    WiFiManagerParameter customMqttUsername("mqtt_user", "MQTT用户名", mqttUsernameBuffer, sizeof(mqttUsernameBuffer));
    WiFiManagerParameter customMqttPassword("mqtt_pass", "MQTT密码", mqttPasswordBuffer, sizeof(mqttPasswordBuffer), "type=\"password\"");
    WiFiManagerParameter customTemperatureOffset("temp_offset", "温度校准(摄氏度)", temperatureOffsetBuffer, sizeof(temperatureOffsetBuffer));
    WiFiManagerParameter customHumidityOffset("humidity_offset", "湿度校准(百分比)", humidityOffsetBuffer, sizeof(humidityOffsetBuffer));
    WiFiManagerParameter customTimezoneHours("timezone_hours", "时区小时(例如8.0)", timezoneHoursBuffer, sizeof(timezoneHoursBuffer));
    WiFiManagerParameter customFactoryReset("factory_reset", "恢复出厂确认(输入RESET)", factoryResetBuffer, sizeof(factoryResetBuffer));

    wifiManager.addParameter(&customPortalInfo);
    wifiManager.addParameter(&customFriendlyName);
    wifiManager.addParameter(&customMqttHost);
    wifiManager.addParameter(&customMqttPort);
    wifiManager.addParameter(&customMqttUsername);
    wifiManager.addParameter(&customMqttPassword);
    wifiManager.addParameter(&customTemperatureOffset);
    wifiManager.addParameter(&customHumidityOffset);
    wifiManager.addParameter(&customTimezoneHours);
    wifiManager.addParameter(&customFactoryReset);

    const String apName = "ESP8266-Config-" + getChipHexId();
    const bool wifiConnected = forcePortal
                                   ? wifiManager.startConfigPortal(apName.c_str(), CONFIG_AP_PASSWORD)
                                   : wifiManager.autoConnect(apName.c_str(), CONFIG_AP_PASSWORD);

    copyString(appConfig.friendlyName, sizeof(appConfig.friendlyName), customFriendlyName.getValue());
    copyString(appConfig.mqttHost, sizeof(appConfig.mqttHost), customMqttHost.getValue());
    copyString(appConfig.mqttPort, sizeof(appConfig.mqttPort), customMqttPort.getValue());
    copyString(appConfig.mqttUsername, sizeof(appConfig.mqttUsername), customMqttUsername.getValue());
    copyString(appConfig.mqttPassword, sizeof(appConfig.mqttPassword), customMqttPassword.getValue());
    copyString(appConfig.temperatureOffset, sizeof(appConfig.temperatureOffset), customTemperatureOffset.getValue());
    copyString(appConfig.humidityOffset, sizeof(appConfig.humidityOffset), customHumidityOffset.getValue());
    copyString(appConfig.timezoneHours, sizeof(appConfig.timezoneHours), customTimezoneHours.getValue());
    appConfig.magic = CONFIG_MAGIC;

    String factoryResetValue = String(customFactoryReset.getValue());
    factoryResetValue.trim();
    factoryResetValue.toUpperCase();
    if (factoryResetValue == "RESET") {
      factoryResetAndRestart("PORTAL_FACTORY_RESET");
      return false;
    }

    char validationReason[96];
    configValid = validateCurrentConfig(validationReason, sizeof(validationReason));
    setLastConfigError(configValid ? "" : validationReason);

    if (!configValid) {
      Serial.print("配置校验失败: ");
      Serial.println(validationReason);
      showDisplayMessage("Config Invalid", validationReason, "Fix and save", "again");
      setLastError("CONFIG_INVALID");
      forcePortal = true;
      continue;
    }

    const bool configChanged = memcmp(&previousConfig, &appConfig, sizeof(PersistentConfig)) != 0;
    if (shouldSaveConfig || configChanged) {
      const bool saved = saveConfigToEeprom();
      Serial.println(saved ? "配置已保存到Flash" : "配置保存失败");
    }

    if (wifiConnected) {
      const String configuredName = getFriendlyName();
      WiFi.hostname(configuredName.c_str());
      configureNtpIfNeeded(true);
      showDisplayMessage("WiFi Connected",
                         getFriendlyName(),
                         WiFi.localIP().toString(),
                         "Config OK");
      Serial.println("Wi-Fi 已连接");
      Serial.print("本机 IP: ");
      Serial.println(WiFi.localIP());
      setLastError("OK");
      return true;
    }

    showDisplayMessage("WiFi Not Ready",
                       getFriendlyName(),
                       "Retry later",
                       "Portal timeout");
    setLastError("WIFI_CONNECT_FAILED");
    return false;
  }
}

void applySwitchState(bool on) {
  switchState = on;
  const uint8_t level = (SWITCH_ACTIVE_HIGH == on) ? HIGH : LOW;
  digitalWrite(PIN_SWITCH, level);
}

void publishSwitchState() {
  mqttClient.publish(TOPIC_SWITCH_STATE, switchState ? "ON" : "OFF", true);
}

void publishOnlineState() {
  mqttClient.publish(TOPIC_STATUS, "online", true);
}

void publishSensorValues() {
  if (!isnan(temperatureDs18b20)) {
    char buffer[16];
    dtostrf(temperatureDs18b20, 0, 2, buffer);
    mqttClient.publish(TOPIC_TEMPERATURE, buffer, true);
  }

  if (!isnan(humidityDht11)) {
    char buffer[16];
    dtostrf(humidityDht11, 0, 1, buffer);
    mqttClient.publish(TOPIC_HUMIDITY, buffer, true);
  }
}

void publishDeviceInfo() {
  if (!mqttClient.connected()) {
    return;
  }

  const String safeFriendlyName = sanitizeForJson(getFriendlyName());
  const String macAddress = sanitizeForJson(WiFi.macAddress());
  char payload[512];

  snprintf(payload,
           sizeof(payload),
           "{\"device_id\":\"%s\",\"device_name\":\"%s\",\"serial_number\":\"%s\",\"model\":\"%s\","
           "\"hardware_revision\":\"%s\",\"firmware_version\":\"%s\",\"mac\":\"%s\"}",
           runtimeDeviceId.c_str(),
           safeFriendlyName.c_str(),
           deviceSerialNumber.c_str(),
           DEVICE_MODEL,
           HARDWARE_REVISION,
           FIRMWARE_VERSION,
           macAddress.c_str());

  mqttClient.publish(TOPIC_DEVICE_INFO, payload, true);
}

void publishSystemState() {
  if (!mqttClient.connected()) {
    return;
  }

  const String safeFriendlyName = sanitizeForJson(getFriendlyName());
  const String safeResetReason = sanitizeForJson(bootResetReason);
  const String safeCurrentError = sanitizeForJson(String(lastErrorText));
  const String safeCurrentConfigError = sanitizeForJson(String(lastConfigErrorText));
  const String safeIp = WiFi.isConnected() ? ipToString(WiFi.localIP()) : String("0.0.0.0");
  const String safeSsid = WiFi.isConnected() ? sanitizeForJson(WiFi.SSID()) : String("");
  const String safeLocalTime = sanitizeForJson(formatLocalTimeString());
  const long rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;

  char payload[1280];
  snprintf(payload,
           sizeof(payload),
           "{\"device_id\":\"%s\",\"device_name\":\"%s\",\"serial_number\":\"%s\",\"model\":\"%s\","
           "\"firmware_version\":\"%s\",\"online\":true,\"config_valid\":%s,\"wifi_connected\":%s,"
           "\"mqtt_connected\":%s,\"oled_ready\":%s,\"ds18b20_ok\":%s,\"dht11_ok\":%s,"
           "\"time_synced\":%s,\"local_time\":\"%s\",\"timezone_hours\":%.1f,"
           "\"temperature_offset\":%.1f,\"humidity_offset\":%.1f,"
           "\"ip\":\"%s\",\"wifi_ssid\":\"%s\",\"wifi_rssi\":%ld,\"uptime_sec\":%lu,"
           "\"reset_reason\":\"%s\",\"last_error\":\"%s\",\"config_error\":\"%s\"}",
           runtimeDeviceId.c_str(),
           safeFriendlyName.c_str(),
           deviceSerialNumber.c_str(),
           DEVICE_MODEL,
           FIRMWARE_VERSION,
           configValid ? "true" : "false",
           WiFi.isConnected() ? "true" : "false",
           mqttClient.connected() ? "true" : "false",
           displayReady ? "true" : "false",
           ds18b20Ok ? "true" : "false",
           dht11Ok ? "true" : "false",
           timeSynced ? "true" : "false",
           safeLocalTime.c_str(),
           getTimezoneHours(),
           getTemperatureOffset(),
           getHumidityOffset(),
           safeIp.c_str(),
           safeSsid.c_str(),
           rssi,
           millis() / 1000UL,
           safeResetReason.c_str(),
           safeCurrentError.c_str(),
           safeCurrentConfigError.c_str());

  mqttClient.publish(TOPIC_SYSTEM_STATE, payload, true);
}

// 根据当前状态生成一个总的健康结论
void refreshErrorSummary() {
  if (!configValid) {
    setLastError("CONFIG_INVALID");
    return;
  }

  if (!displayReady) {
    setLastError("OLED_NOT_READY");
    return;
  }

  if (!WiFi.isConnected()) {
    setLastError("WIFI_DISCONNECTED");
    return;
  }

  if (!isMqttConfigured()) {
    setLastError("MQTT_CONFIG_MISSING");
    return;
  }

  if (!mqttClient.connected()) {
    setLastError("MQTT_DISCONNECTED");
    return;
  }

  if (!ds18b20Ok) {
    setLastError("DS18B20_READ_FAIL");
    return;
  }

  if (!dht11Ok) {
    setLastError("DHT11_READ_FAIL");
    return;
  }

  setLastError("OK");
}

// OLED 页面轮播显示
// 第 1 页看温湿度
// 第 2 页看系统状态
// 出错时第 3 页显示故障信息
void renderDisplay() {
  if (!displayReady) {
    return;
  }

  const bool hasFault = strcmp(lastErrorText, "OK") != 0;
  const unsigned long pageCount = hasFault ? 4UL : 3UL;
  const unsigned long pageIndex = (millis() / DISPLAY_PAGE_INTERVAL_MS) % pageCount;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (pageIndex == 0) {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(getFriendlyName());

    display.setTextSize(2);
    display.setCursor(0, 16);
    display.print("T:");
    if (isnan(temperatureDs18b20)) {
      display.print("--.-");
    } else {
      display.print(temperatureDs18b20, 1);
    }
    display.println("C");

    display.setCursor(0, 38);
    display.print("H:");
    if (isnan(humidityDht11)) {
      display.print("--.-");
    } else {
      display.print(humidityDht11, 0);
    }
    display.println("%");

    display.setTextSize(1);
    display.setCursor(0, 56);
    display.print("SW:");
    display.print(switchState ? "ON " : "OFF");
    display.print(" SN:");
    display.print(getChipHexId());
  } else if (pageIndex == 1) {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("WiFi:");
    display.println(WiFi.isConnected() ? "OK" : "OFF");

    display.setCursor(64, 0);
    display.print("MQTT:");
    display.println(mqttClient.connected() ? "OK" : "OFF");

    display.setCursor(0, 18);
    display.print("RSSI:");
    if (WiFi.isConnected()) {
      display.print(WiFi.RSSI());
      display.println("dBm");
    } else {
      display.println("--");
    }

    display.setCursor(0, 32);
    display.print("IP:");
    if (WiFi.isConnected()) {
      display.println(ipToString(WiFi.localIP()));
    } else {
      display.println("0.0.0.0");
    }

    display.setCursor(0, 46);
    display.print("Up:");
    display.print(millis() / 1000UL);
    display.println("s");

    display.setCursor(0, 56);
    display.print("FW:");
    display.print(FIRMWARE_VERSION);
  } else if (pageIndex == 2) {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Time / Clock");
    display.setTextSize(2);
    display.setCursor(0, 18);
    display.println(formatLocalTimeShort());
    display.setTextSize(1);
    display.setCursor(0, 44);
    if (timeSynced) {
      display.println(formatLocalTimeString().substring(0, 10));
    } else {
      display.println("NTP syncing...");
    }
    display.setCursor(0, 56);
    display.print("TZ:");
    display.print(getTimezoneHours(), 1);
    display.print("  T:");
    display.print(getTemperatureOffset(), 1);
    display.print(" H:");
    display.print(getHumidityOffset(), 1);
  } else {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Fault / Recover");
    display.setCursor(0, 16);
    display.println(lastErrorText);
    display.setCursor(0, 30);
    display.print("DS:");
    display.print(ds18b20Ok ? "OK " : "ERR");
    display.print(" DHT:");
    display.println(dht11Ok ? "OK" : "ERR");
    display.setCursor(0, 44);
    display.print("CFG:");
    display.println(configValid ? "OK" : "ERR");
    display.setCursor(0, 56);
    display.println("Auto retry active");
  }

  display.display();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);

  for (unsigned int i = 0; i < length; i++) {
    message += static_cast<char>(payload[i]);
  }

  message.trim();

  if (String(topic) == TOPIC_SWITCH_SET) {
    if (message.equalsIgnoreCase("ON")) {
      applySwitchState(true);
      publishSwitchState();
      publishSystemEvent("SWITCH_ON");
    } else if (message.equalsIgnoreCase("OFF")) {
      applySwitchState(false);
      publishSwitchState();
      publishSystemEvent("SWITCH_OFF");
    }
    return;
  }

  if (String(topic) == TOPIC_SYSTEM_RESET) {
    if (message.equalsIgnoreCase("RESET")) {
      publishSystemEvent("REMOTE_FACTORY_RESET");
      factoryResetAndRestart("REMOTE_FACTORY_RESET");
    } else if (message.equalsIgnoreCase("RESTART")) {
      publishSystemEvent("REMOTE_RESTART");
      restartDevice("REMOTE_RESTART");
    }
  }
}

void connectWiFiIfNeeded() {
  if (WiFi.isConnected()) {
    return;
  }

  const unsigned long now = millis();
  if (now - lastWifiRetryMs < WIFI_RETRY_MS) {
    return;
  }

  lastWifiRetryMs = now;
  WiFi.mode(WIFI_STA);
  WiFi.begin();
}

void connectMqttIfNeeded() {
  if (!WiFi.isConnected() || mqttClient.connected() || !isMqttConfigured()) {
    return;
  }

  const unsigned long now = millis();
  if (now - lastMqttRetryMs < MQTT_RETRY_MS) {
    return;
  }

  lastMqttRetryMs = now;

  bool connected = false;

  if (appConfig.mqttUsername[0] == '\0') {
    connected = mqttClient.connect(
        runtimeDeviceId.c_str(),
        TOPIC_STATUS,
        1,
        true,
        "offline");
  } else {
    connected = mqttClient.connect(
        runtimeDeviceId.c_str(),
        appConfig.mqttUsername,
        appConfig.mqttPassword,
        TOPIC_STATUS,
        1,
        true,
        "offline");
  }

  if (!connected) {
    return;
  }

  mqttClient.subscribe(TOPIC_SWITCH_SET);
  mqttClient.subscribe(TOPIC_SYSTEM_RESET);
  publishOnlineState();
  publishSwitchState();
  publishSensorValues();
  publishDeviceInfo();
  publishSystemState();
  publishSystemEvent("MQTT_CONNECTED");
}

// 读取两个传感器
// DS18B20 作为主温度值
// DHT11 主要用于湿度，温度值这里只保留方便串口调试
void readSensors() {
  ds18b20.requestTemperatures();
  const float dsValue = ds18b20.getTempCByIndex(0);
  if (dsValue > -100.0f && dsValue < 125.0f) {
    temperatureDs18b20 = dsValue + getTemperatureOffset();
    ds18b20Ok = true;
  } else {
    temperatureDs18b20 = NAN;
    ds18b20Ok = false;
  }

  const float dhtHumidity = dht.readHumidity();
  const float dhtTemperature = dht.readTemperature();

  humidityDht11 = isnan(dhtHumidity) ? NAN : (dhtHumidity + getHumidityOffset());
  temperatureDht11 = isnan(dhtTemperature) ? NAN : (dhtTemperature + getTemperatureOffset());
  dht11Ok = !isnan(humidityDht11);
}

void setupDisplay() {
  pinMode(PIN_OLED_RESET, OUTPUT);
  pinMode(PIN_OLED_CS, OUTPUT);
  pinMode(PIN_OLED_DC, OUTPUT);

  displayReady = display.begin(SSD1306_SWITCHCAPVCC);
  if (!displayReady) {
    Serial.println("OLED 初始化失败");
    return;
  }

  showDisplayMessage("Booting...", getFriendlyName(), DEVICE_MODEL, FIRMWARE_VERSION);
}

void recoverDisplayIfNeeded() {
  if (displayReady) {
    return;
  }

  const unsigned long now = millis();
  if (now - lastDisplayRecoveryMs < DISPLAY_RECOVERY_MS) {
    return;
  }

  lastDisplayRecoveryMs = now;
  Serial.println("尝试重新初始化 OLED");
  setupDisplay();
}

void handleRecoveryPolicies() {
  const unsigned long now = millis();

  if (!WiFi.isConnected()) {
    if (wifiDisconnectSinceMs == 0) {
      wifiDisconnectSinceMs = now;
      publishSystemEvent("WIFI_DISCONNECTED");
    }

    if (now - wifiDisconnectSinceMs >= WIFI_RECOVERY_PORTAL_MS) {
      enterRecoveryConfigPortal("WIFI_RECOVERY_PORTAL");
      return;
    }
  } else {
    wifiDisconnectSinceMs = 0;
  }

  if (WiFi.isConnected() && isMqttConfigured() && !mqttClient.connected()) {
    if (mqttDisconnectSinceMs == 0) {
      mqttDisconnectSinceMs = now;
    }

    if (now - mqttDisconnectSinceMs >= MQTT_RECOVERY_PORTAL_MS) {
      enterRecoveryConfigPortal("MQTT_RECOVERY_PORTAL");
      return;
    }
  } else {
    mqttDisconnectSinceMs = 0;
  }

  configureNtpIfNeeded();
  recoverDisplayIfNeeded();
}

void printSerialStatus() {
  Serial.print("Device: ");
  Serial.print(getFriendlyName());
  Serial.print(" | IP: ");
  Serial.print(WiFi.isConnected() ? ipToString(WiFi.localIP()) : "not connected");
  Serial.print(" | RSSI: ");
  Serial.print(WiFi.isConnected() ? String(WiFi.RSSI()) : String("--"));
  Serial.print(" | DS18B20: ");
  Serial.print(temperatureDs18b20);
  Serial.print(" C | DHT11 Humi: ");
  Serial.print(humidityDht11);
  Serial.print(" % | Switch: ");
  Serial.print(switchState ? "ON" : "OFF");
  Serial.print(" | Time: ");
  Serial.print(formatLocalTimeShort());
  Serial.print(" | Error: ");
  Serial.println(lastErrorText);
}

void setupRuntimeIdentity() {
  deviceSerialNumber = "SN-" + getChipHexId();
  runtimeDeviceId = String(DEVICE_ID) + "-" + getChipHexId();
  bootResetReason = ESP.getResetReason();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  setupRuntimeIdentity();
  resetAppConfigToDefaults();
  loadConfigFromEeprom();

  char validationReason[96];
  configValid = validateCurrentConfig(validationReason, sizeof(validationReason));
  setLastConfigError(configValid ? "" : validationReason);
  refreshErrorSummary();

  pinMode(PIN_SWITCH, OUTPUT);
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
  refreshErrorSummary();
}

void loop() {
  connectWiFiIfNeeded();
  connectMqttIfNeeded();

  if (mqttClient.connected()) {
    mqttClient.loop();
  }

  handleRecoveryPolicies();

  const bool previousTimeSynced = timeSynced;
  timeSynced = isTimeAvailable();
  if (!previousTimeSynced && timeSynced && mqttClient.connected()) {
    publishSystemEvent("NTP_TIME_SYNCED");
    publishSystemState();
  }

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
