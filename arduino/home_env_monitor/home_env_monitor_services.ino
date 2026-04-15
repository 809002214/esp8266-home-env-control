// ---------------------------
// 服务模块（由主 ino 拆分）
// 包含：配置、配网、MQTT、OTA、显示、传感器、恢复策略等函数
// 目的：让 home_env_monitor.ino 主文件只保留入口流程
// ---------------------------

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

uint16_t getOtaPort() {
  long port = strtol(appConfig.otaPort, nullptr, 10);
  if (port <= 0 || port > 65535) {
    return 1880;
  }
  return static_cast<uint16_t>(port);
}

String getOtaPath() {
  if (appConfig.otaPath[0] == '\0') {
    return String("/firmware/esp8266-home-env-control.bin");
  }
  return String(appConfig.otaPath);
}

bool isPrintableAscii(const char* value) {
  if (value == nullptr) {
    return false;
  }

  for (size_t i = 0; value[i] != '\0'; i++) {
    const char c = value[i];
    if (c < 32 || c > 126) {
      return false;
    }
  }

  return true;
}

bool isValidOtaPath(const char* value) {
  if (value == nullptr || value[0] == '\0' || value[0] != '/') {
    return false;
  }

  if (!isPrintableAscii(value)) {
    return false;
  }

  for (size_t i = 0; value[i] != '\0'; i++) {
    const char c = value[i];
    if (c == '"' || c == '\\') {
      return false;
    }
  }

  return true;
}

uint8_t parseHourOrDefault(const char* value, uint8_t defaultValue) {
  if (!isAllDigits(value)) {
    return defaultValue;
  }

  const long parsed = strtol(value, nullptr, 10);
  if (parsed < 0 || parsed > 23) {
    return defaultValue;
  }

  return static_cast<uint8_t>(parsed);
}

// ---------- 夜间模式与远程恢复出厂安全 ----------

uint8_t getNightStartHour() {
  return parseHourOrDefault(appConfig.nightStartHour, 23);
}

uint8_t getNightEndHour() {
  return parseHourOrDefault(appConfig.nightEndHour, 7);
}

bool isRemoteFactoryResetEnabled() {
  return appConfig.remoteFactoryResetEnabled[0] == '1';
}

bool isResetArmActive() {
  if (remoteResetArmUntilMs == 0) {
    return false;
  }
  return static_cast<long>(remoteResetArmUntilMs - millis()) > 0;
}

void setBoardLed(bool on) {
  // NodeMCU 板载蓝灯是低电平点亮
  digitalWrite(PIN_BOARD_LED, on ? LOW : HIGH);
}

void setDisplayPanelPower(bool on) {
  if (!displayReady || displayPanelOn == on) {
    return;
  }

  display.ssd1306_command(on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
  displayPanelOn = on;
}

bool isNightModeTimeNow() {
  if (!timeSynced) {
    return false;
  }

  time_t now = time(nullptr);
  struct tm timeInfo;
  localtime_r(&now, &timeInfo);
  const uint8_t currentHour = static_cast<uint8_t>(timeInfo.tm_hour);
  const uint8_t startHour = getNightStartHour();
  const uint8_t endHour = getNightEndHour();

  if (startHour == endHour) {
    return false;
  }

  if (startHour < endHour) {
    return currentHour >= startHour && currentHour < endHour;
  }

  return currentHour >= startHour || currentHour < endHour;
}

// ---------- 时间与字符串工具 ----------

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

// ---------- 配置读写与校验 ----------

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
  appConfig.otaPort[sizeof(appConfig.otaPort) - 1] = '\0';
  appConfig.otaPath[sizeof(appConfig.otaPath) - 1] = '\0';
  appConfig.nightStartHour[sizeof(appConfig.nightStartHour) - 1] = '\0';
  appConfig.nightEndHour[sizeof(appConfig.nightEndHour) - 1] = '\0';
  appConfig.remoteFactoryResetEnabled[sizeof(appConfig.remoteFactoryResetEnabled) - 1] = '\0';

  if (!isAllDigits(appConfig.otaPort)) {
    copyString(appConfig.otaPort, sizeof(appConfig.otaPort), "1880");
  } else {
    const long otaPort = strtol(appConfig.otaPort, nullptr, 10);
    if (otaPort <= 0 || otaPort > 65535) {
      copyString(appConfig.otaPort, sizeof(appConfig.otaPort), "1880");
    }
  }

  if (!isValidOtaPath(appConfig.otaPath)) {
    copyString(appConfig.otaPath, sizeof(appConfig.otaPath), "/firmware/esp8266-home-env-control.bin");
  }

  if (!isAllDigits(appConfig.nightStartHour)) {
    copyString(appConfig.nightStartHour, sizeof(appConfig.nightStartHour), "23");
  } else {
    const long nightStartHour = strtol(appConfig.nightStartHour, nullptr, 10);
    if (nightStartHour < 0 || nightStartHour > 23) {
      copyString(appConfig.nightStartHour, sizeof(appConfig.nightStartHour), "23");
    }
  }

  if (!isAllDigits(appConfig.nightEndHour)) {
    copyString(appConfig.nightEndHour, sizeof(appConfig.nightEndHour), "7");
  } else {
    const long nightEndHour = strtol(appConfig.nightEndHour, nullptr, 10);
    if (nightEndHour < 0 || nightEndHour > 23) {
      copyString(appConfig.nightEndHour, sizeof(appConfig.nightEndHour), "7");
    }
  }

  const bool resetFlagValid = (appConfig.remoteFactoryResetEnabled[0] == '0' || appConfig.remoteFactoryResetEnabled[0] == '1') &&
                              appConfig.remoteFactoryResetEnabled[1] == '\0';
  if (!resetFlagValid) {
    copyString(appConfig.remoteFactoryResetEnabled, sizeof(appConfig.remoteFactoryResetEnabled), "0");
  }

  if (appConfig.otaPort[0] == '\0') {
    copyString(appConfig.otaPort, sizeof(appConfig.otaPort), "1880");
  }

  if (appConfig.otaPath[0] == '\0') {
    copyString(appConfig.otaPath, sizeof(appConfig.otaPath), "/firmware/esp8266-home-env-control.bin");
  }

  if (appConfig.nightStartHour[0] == '\0') {
    copyString(appConfig.nightStartHour, sizeof(appConfig.nightStartHour), "23");
  }

  if (appConfig.nightEndHour[0] == '\0') {
    copyString(appConfig.nightEndHour, sizeof(appConfig.nightEndHour), "7");
  }

  if (appConfig.remoteFactoryResetEnabled[0] == '\0') {
    copyString(appConfig.remoteFactoryResetEnabled, sizeof(appConfig.remoteFactoryResetEnabled), "0");
  }
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
  copyString(appConfig.otaPort, sizeof(appConfig.otaPort), "1880");
  copyString(appConfig.otaPath, sizeof(appConfig.otaPath), "/firmware/esp8266-home-env-control.bin");
  copyString(appConfig.nightStartHour, sizeof(appConfig.nightStartHour), "23");
  copyString(appConfig.nightEndHour, sizeof(appConfig.nightEndHour), "7");
  copyString(appConfig.remoteFactoryResetEnabled, sizeof(appConfig.remoteFactoryResetEnabled), "0");
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

  if (!isAllDigits(appConfig.otaPort)) {
    copyString(reasonBuffer, reasonBufferSize, "OTA端口必须是数字");
    return false;
  }

  const long otaPort = strtol(appConfig.otaPort, nullptr, 10);
  if (otaPort <= 0 || otaPort > 65535) {
    copyString(reasonBuffer, reasonBufferSize, "OTA端口范围必须在1到65535");
    return false;
  }

  if (strlen(appConfig.otaPath) == 0 || appConfig.otaPath[0] != '/') {
    copyString(reasonBuffer, reasonBufferSize, "OTA路径必须以/开头");
    return false;
  }

  if (!isAllDigits(appConfig.nightStartHour)) {
    copyString(reasonBuffer, reasonBufferSize, "夜间开始小时必须是数字");
    return false;
  }

  const long nightStartHour = strtol(appConfig.nightStartHour, nullptr, 10);
  if (nightStartHour < 0 || nightStartHour > 23) {
    copyString(reasonBuffer, reasonBufferSize, "夜间开始小时范围0到23");
    return false;
  }

  if (!isAllDigits(appConfig.nightEndHour)) {
    copyString(reasonBuffer, reasonBufferSize, "夜间结束小时必须是数字");
    return false;
  }

  const long nightEndHour = strtol(appConfig.nightEndHour, nullptr, 10);
  if (nightEndHour < 0 || nightEndHour > 23) {
    copyString(reasonBuffer, reasonBufferSize, "夜间结束小时范围0到23");
    return false;
  }

  if (!(appConfig.remoteFactoryResetEnabled[0] == '0' || appConfig.remoteFactoryResetEnabled[0] == '1') ||
      appConfig.remoteFactoryResetEnabled[1] != '\0') {
    copyString(reasonBuffer, reasonBufferSize, "远程恢复开关只能是0或1");
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

// ---------- 显示与配网入口 ----------

void showDisplayMessage(const String& line1,
                        const String& line2 = "",
                        const String& line3 = "",
                        const String& line4 = "") {
  if (!displayReady) {
    return;
  }

  setDisplayPanelPower(true);
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
  setupArduinoOtaIfNeeded();

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
  html += "<span>夜间模式会在设定时间自动关闭OLED和板载蓝灯。</span><br>";
  html += "<span>远程恢复出厂默认禁用，建议保持0。</span><br>";
  html += "<span>HTTP OTA 默认会从 MQTT 同一台主机下载固件。</span><br>";
  html += "<span>默认 OTA 地址示例：http://" + String(appConfig.mqttHost) + ":" + String(getOtaPort()) + getOtaPath() + "</span><br>";
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
    char otaPortBuffer[sizeof(appConfig.otaPort)];
    char otaPathBuffer[sizeof(appConfig.otaPath)];
    char nightStartHourBuffer[sizeof(appConfig.nightStartHour)];
    char nightEndHourBuffer[sizeof(appConfig.nightEndHour)];
    char remoteFactoryResetEnabledBuffer[sizeof(appConfig.remoteFactoryResetEnabled)];
    char factoryResetBuffer[8] = "";

    copyString(friendlyNameBuffer, sizeof(friendlyNameBuffer), getFriendlyName().c_str());
    copyString(mqttHostBuffer, sizeof(mqttHostBuffer), appConfig.mqttHost);
    copyString(mqttPortBuffer, sizeof(mqttPortBuffer), appConfig.mqttPort);
    copyString(mqttUsernameBuffer, sizeof(mqttUsernameBuffer), appConfig.mqttUsername);
    copyString(mqttPasswordBuffer, sizeof(mqttPasswordBuffer), appConfig.mqttPassword);
    copyString(temperatureOffsetBuffer, sizeof(temperatureOffsetBuffer), appConfig.temperatureOffset);
    copyString(humidityOffsetBuffer, sizeof(humidityOffsetBuffer), appConfig.humidityOffset);
    copyString(timezoneHoursBuffer, sizeof(timezoneHoursBuffer), appConfig.timezoneHours);
    copyString(otaPortBuffer, sizeof(otaPortBuffer), appConfig.otaPort);
    copyString(otaPathBuffer, sizeof(otaPathBuffer), appConfig.otaPath);
    copyString(nightStartHourBuffer, sizeof(nightStartHourBuffer), appConfig.nightStartHour);
    copyString(nightEndHourBuffer, sizeof(nightEndHourBuffer), appConfig.nightEndHour);
    copyString(remoteFactoryResetEnabledBuffer, sizeof(remoteFactoryResetEnabledBuffer), appConfig.remoteFactoryResetEnabled);

    WiFiManagerParameter customPortalInfo(portalInfoHtml.c_str());
    WiFiManagerParameter customFriendlyName("friendly_name", "设备名", friendlyNameBuffer, sizeof(friendlyNameBuffer));
    WiFiManagerParameter customMqttHost("mqtt_host", "MQTT服务器地址", mqttHostBuffer, sizeof(mqttHostBuffer));
    WiFiManagerParameter customMqttPort("mqtt_port", "MQTT端口", mqttPortBuffer, sizeof(mqttPortBuffer));
    WiFiManagerParameter customMqttUsername("mqtt_user", "MQTT用户名", mqttUsernameBuffer, sizeof(mqttUsernameBuffer));
    WiFiManagerParameter customMqttPassword("mqtt_pass", "MQTT密码", mqttPasswordBuffer, sizeof(mqttPasswordBuffer), "type=\"password\"");
    WiFiManagerParameter customTemperatureOffset("temp_offset", "温度校准(摄氏度)", temperatureOffsetBuffer, sizeof(temperatureOffsetBuffer));
    WiFiManagerParameter customHumidityOffset("humidity_offset", "湿度校准(百分比)", humidityOffsetBuffer, sizeof(humidityOffsetBuffer));
    WiFiManagerParameter customTimezoneHours("timezone_hours", "时区小时(例如8.0)", timezoneHoursBuffer, sizeof(timezoneHoursBuffer));
    WiFiManagerParameter customOtaPort("ota_port", "OTA下载端口", otaPortBuffer, sizeof(otaPortBuffer));
    WiFiManagerParameter customOtaPath("ota_path", "OTA固件路径(以/开头)", otaPathBuffer, sizeof(otaPathBuffer));
    WiFiManagerParameter customNightStartHour("night_start_hour", "夜间开始小时(0-23)", nightStartHourBuffer, sizeof(nightStartHourBuffer));
    WiFiManagerParameter customNightEndHour("night_end_hour", "夜间结束小时(0-23)", nightEndHourBuffer, sizeof(nightEndHourBuffer));
    WiFiManagerParameter customRemoteFactoryReset("remote_factory_reset", "允许远程恢复出厂(0禁用/1启用)", remoteFactoryResetEnabledBuffer, sizeof(remoteFactoryResetEnabledBuffer));
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
    wifiManager.addParameter(&customOtaPort);
    wifiManager.addParameter(&customOtaPath);
    wifiManager.addParameter(&customNightStartHour);
    wifiManager.addParameter(&customNightEndHour);
    wifiManager.addParameter(&customRemoteFactoryReset);
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
    copyString(appConfig.otaPort, sizeof(appConfig.otaPort), customOtaPort.getValue());
    copyString(appConfig.otaPath, sizeof(appConfig.otaPath), customOtaPath.getValue());
    copyString(appConfig.nightStartHour, sizeof(appConfig.nightStartHour), customNightStartHour.getValue());
    copyString(appConfig.nightEndHour, sizeof(appConfig.nightEndHour), customNightEndHour.getValue());
    copyString(appConfig.remoteFactoryResetEnabled, sizeof(appConfig.remoteFactoryResetEnabled), customRemoteFactoryReset.getValue());
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

// ---------- MQTT 上报与命令处理 ----------

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

void publishOtaState() {
  if (!mqttClient.connected()) {
    return;
  }

  const String safeState = sanitizeForJson(String(otaStateText));
  const String safePath = sanitizeForJson(getOtaPath());
  char payload[384];
  snprintf(payload,
           sizeof(payload),
           "{\"device_id\":\"%s\",\"device_name\":\"%s\",\"firmware_version\":\"%s\","
           "\"ota_in_progress\":%s,\"arduino_ota_ready\":%s,\"ota_state\":\"%s\","
           "\"ota_port\":%u,\"ota_path\":\"%s\"}",
           runtimeDeviceId.c_str(),
           sanitizeForJson(getFriendlyName()).c_str(),
           FIRMWARE_VERSION,
           otaInProgress ? "true" : "false",
           arduinoOtaReady ? "true" : "false",
           safeState.c_str(),
           getOtaPort(),
           safePath.c_str());

  mqttClient.publish(TOPIC_OTA_STATE, payload, true);
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
  const String safeOtaState = sanitizeForJson(String(otaStateText));
  const String safeOtaPath = sanitizeForJson(getOtaPath());
  const long rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;

  char payload[1536];
  snprintf(payload,
           sizeof(payload),
           "{\"device_id\":\"%s\",\"device_name\":\"%s\",\"serial_number\":\"%s\",\"model\":\"%s\","
           "\"firmware_version\":\"%s\",\"online\":true,\"config_valid\":%s,\"wifi_connected\":%s,"
           "\"mqtt_connected\":%s,\"oled_ready\":%s,\"ds18b20_ok\":%s,\"dht11_ok\":%s,"
           "\"time_synced\":%s,\"local_time\":\"%s\",\"timezone_hours\":%.1f,"
           "\"temperature_offset\":%.1f,\"humidity_offset\":%.1f,"
           "\"ota_in_progress\":%s,\"arduino_ota_ready\":%s,\"ota_state\":\"%s\","
           "\"ota_port\":%u,\"ota_path\":\"%s\","
           "\"night_mode_active\":%s,\"night_start_hour\":%u,\"night_end_hour\":%u,"
           "\"remote_factory_reset_enabled\":%s,\"remote_factory_reset_armed\":%s,"
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
           otaInProgress ? "true" : "false",
           arduinoOtaReady ? "true" : "false",
           safeOtaState.c_str(),
           getOtaPort(),
           safeOtaPath.c_str(),
           nightModeActive ? "true" : "false",
           getNightStartHour(),
           getNightEndHour(),
           isRemoteFactoryResetEnabled() ? "true" : "false",
           isResetArmActive() ? "true" : "false",
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
  if (otaInProgress) {
    setLastError("OTA_UPDATING");
    return;
  }

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

// ---------- 运行时策略（夜间模式、显示、OTA、恢复） ----------

void updateNightModeIfNeeded() {
  bool shouldNightMode = isNightModeTimeNow();
  if (otaInProgress) {
    shouldNightMode = false;
  }

  if (shouldNightMode == nightModeActive) {
    return;
  }

  nightModeActive = shouldNightMode;
  setBoardLed(!nightModeActive);
  setDisplayPanelPower(!nightModeActive);

  if (mqttClient.connected()) {
    publishSystemEvent(nightModeActive ? "NIGHT_MODE_ON" : "NIGHT_MODE_OFF");
  }
}

// OLED 页面轮播显示（模块化）
// 具体绘制逻辑在 display_pages.cpp
void renderDisplay() {
  if (!displayReady) {
    return;
  }

  if (nightModeActive && !otaInProgress) {
    return;
  }

  String ip = WiFi.isConnected() ? ipToString(WiFi.localIP()) : String("0.0.0.0");
  String localDate = timeSynced ? formatLocalTimeString().substring(0, 10) : String("NTP WAIT");

  DisplaySnapshot snapshot;
  snapshot.friendlyName = getFriendlyName();
  snapshot.chipId = getChipHexId();
  snapshot.firmwareVersion = String(FIRMWARE_VERSION);
  snapshot.ip = ip;
  snapshot.localTimeShort = formatLocalTimeShort();
  snapshot.localDate = localDate;
  snapshot.lastError = String(lastErrorText);
  snapshot.otaState = String(otaStateText);
  snapshot.otaPath = getOtaPath();
  snapshot.wifiConnected = WiFi.isConnected();
  snapshot.mqttConnected = mqttClient.connected();
  snapshot.switchState = switchState;
  snapshot.configValid = configValid;
  snapshot.ds18b20Ok = ds18b20Ok;
  snapshot.dht11Ok = dht11Ok;
  snapshot.timeSynced = timeSynced;
  snapshot.otaInProgress = otaInProgress;
  snapshot.temperature = temperatureDs18b20;
  snapshot.humidity = humidityDht11;
  snapshot.tempOffset = getTemperatureOffset();
  snapshot.humidityOffset = getHumidityOffset();
  snapshot.timezoneHours = getTimezoneHours();
  snapshot.wifiRssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  snapshot.uptimeSec = millis() / 1000UL;
  snapshot.pageIntervalSec = static_cast<uint8_t>(DISPLAY_PAGE_INTERVAL_MS / 1000UL);
  snapshot.otaPort = getOtaPort();
  snapshot.weather = weatherState;

  renderDisplayPages(display, snapshot, millis(), DISPLAY_PAGE_INTERVAL_MS);
}

void setupArduinoOtaIfNeeded() {
  if (arduinoOtaReady || !WiFi.isConnected()) {
    return;
  }

  ArduinoOTA.setHostname(getFriendlyName().c_str());

  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    lastOtaProgressPercent = 255;
    copyString(otaStateText, sizeof(otaStateText), "ARDUINO_OTA_START");
    showDisplayMessage("Arduino OTA", getFriendlyName(), "Uploading...", "");
    publishSystemEvent("ARDUINO_OTA_START");
    publishOtaState();
    publishSystemState();
  });

  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    copyString(otaStateText, sizeof(otaStateText), "ARDUINO_OTA_DONE");
    showDisplayMessage("Arduino OTA", getFriendlyName(), "Upload done", "Restarting...");
    publishSystemEvent("ARDUINO_OTA_DONE");
    publishOtaState();
    publishSystemState();
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total == 0) {
      return;
    }

    const uint8_t percent = static_cast<uint8_t>((progress * 100U) / total);
    if (percent == lastOtaProgressPercent) {
      return;
    }

    lastOtaProgressPercent = percent;
    char progressText[96];
    snprintf(progressText, sizeof(progressText), "ARDUINO_OTA_%u%%", percent);
    copyString(otaStateText, sizeof(otaStateText), progressText);

    if (percent % 10 == 0 || percent >= 95) {
      char line4[24];
      snprintf(line4, sizeof(line4), "%u%%", percent);
      showDisplayMessage("Arduino OTA", getFriendlyName(), "Uploading...", line4);
      publishOtaState();
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    char errorText[96];
    snprintf(errorText, sizeof(errorText), "ARDUINO_OTA_ERROR_%u", static_cast<unsigned>(error));
    copyString(otaStateText, sizeof(otaStateText), errorText);
    showDisplayMessage("Arduino OTA", "Upload failed", errorText, "");
    publishSystemEvent(errorText);
    publishOtaState();
    publishSystemState();
  });

  ArduinoOTA.begin();
  arduinoOtaReady = true;
  copyString(otaStateText, sizeof(otaStateText), "ARDUINO_OTA_READY");
  publishSystemEvent("ARDUINO_OTA_READY");
  publishOtaState();
}

bool performHttpOtaUpdate() {
  return performHttpOtaUpdate(nullptr);
}

bool performHttpOtaUpdate(const char* customPath) {
  if (!WiFi.isConnected()) {
    copyString(otaStateText, sizeof(otaStateText), "OTA_WIFI_OFFLINE");
    publishOtaState();
    return false;
  }

  if (!configValid || appConfig.mqttHost[0] == '\0') {
    copyString(otaStateText, sizeof(otaStateText), "OTA_CONFIG_INVALID");
    publishOtaState();
    return false;
  }

  String otaPath = customPath == nullptr ? getOtaPath() : String(customPath);
  otaPath.trim();
  if (!otaPath.startsWith("/")) {
    otaPath = "/" + otaPath;
  }

  otaInProgress = true;
  copyString(otaStateText, sizeof(otaStateText), "HTTP_OTA_START");
  publishSystemEvent("HTTP_OTA_START");
  publishOtaState();
  publishSystemState();
  showDisplayMessage("HTTP OTA", appConfig.mqttHost, String(getOtaPort()) + otaPath, "Downloading...");

  WiFiClient updateClient;
  ESPhttpUpdate.rebootOnUpdate(false);
  const t_httpUpdate_return updateResult =
      ESPhttpUpdate.update(updateClient, appConfig.mqttHost, getOtaPort(), otaPath.c_str(), FIRMWARE_VERSION);

  otaInProgress = false;

  if (updateResult == HTTP_UPDATE_OK) {
    copyString(otaStateText, sizeof(otaStateText), "HTTP_OTA_SUCCESS");
    publishSystemEvent("HTTP_OTA_SUCCESS");
    publishOtaState();
    publishSystemState();
    showDisplayMessage("HTTP OTA", "Update success", "Restarting...", "");
    delay(1200);
    ESP.restart();
    return true;
  }

  if (updateResult == HTTP_UPDATE_NO_UPDATES) {
    copyString(otaStateText, sizeof(otaStateText), "HTTP_OTA_NO_UPDATE");
    publishSystemEvent("HTTP_OTA_NO_UPDATE");
    publishOtaState();
    publishSystemState();
    showDisplayMessage("HTTP OTA", "No new version", otaPath, "");
    return false;
  }

  const String lastError = sanitizeForJson(ESPhttpUpdate.getLastErrorString());
  char otaError[96];
  snprintf(otaError, sizeof(otaError), "HTTP_OTA_FAIL_%d", ESPhttpUpdate.getLastError());
  copyString(otaStateText, sizeof(otaStateText), otaError);
  setLastError(otaError);
  publishSystemEvent(String(otaError) + ":" + lastError);
  publishOtaState();
  publishSystemState();
  showDisplayMessage("HTTP OTA", "Update failed", lastError, "");
  return false;
}

void updateWeatherStateFromMessage(const String& message) {
  if (!parseWeatherPayload(message, weatherState)) {
    return;
  }

  if (mqttClient.connected()) {
    publishSystemEvent("WEATHER_UPDATED");
  }
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
    if (message.equalsIgnoreCase("RESTART")) {
      publishSystemEvent("REMOTE_RESTART");
      restartDevice("REMOTE_RESTART");
    } else if (message.equalsIgnoreCase("ARM_RESET")) {
      if (!isRemoteFactoryResetEnabled()) {
        publishSystemEvent("REMOTE_FACTORY_RESET_DISABLED");
        return;
      }

      remoteResetArmUntilMs = millis() + RESET_ARM_WINDOW_MS;
      publishSystemEvent("REMOTE_FACTORY_RESET_ARMED");
    } else if (message.equalsIgnoreCase("RESET:CONFIRM")) {
      if (!isRemoteFactoryResetEnabled()) {
        publishSystemEvent("REMOTE_FACTORY_RESET_DISABLED");
        return;
      }

      if (!isResetArmActive()) {
        publishSystemEvent("REMOTE_FACTORY_RESET_NOT_ARMED");
        return;
      }

      remoteResetArmUntilMs = 0;
      publishSystemEvent("REMOTE_FACTORY_RESET_CONFIRMED");
      factoryResetAndRestart("REMOTE_FACTORY_RESET");
    } else if (message.equalsIgnoreCase("RESET")) {
      publishSystemEvent("REMOTE_FACTORY_RESET_REQUIRES_CONFIRM");
    }
    return;
  }

  if (String(topic) == TOPIC_OTA_SET) {
    if (otaInProgress) {
      publishOtaState();
      return;
    }

    if (message.equalsIgnoreCase("UPDATE")) {
      performHttpOtaUpdate();
    } else if (message.startsWith("UPDATE:")) {
      const String customPath = message.substring(7);
      performHttpOtaUpdate(customPath.c_str());
    } else if (message.equalsIgnoreCase("STATE")) {
      publishOtaState();
    }
    return;
  }

  if (String(topic) == TOPIC_WEATHER_STATE) {
    updateWeatherStateFromMessage(message);
    return;
  }
}

// ---------- 连接、采样、诊断 ----------

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
  mqttClient.subscribe(TOPIC_OTA_SET);
  mqttClient.subscribe(TOPIC_WEATHER_STATE);
  publishOnlineState();
  publishSwitchState();
  publishSensorValues();
  publishDeviceInfo();
  publishOtaState();
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

  displayPanelOn = true;
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
  Serial.print(" | Uptime: ");
  Serial.print(formatUptimeZh(millis() / 1000UL));
  Serial.print(" | Error: ");
  Serial.println(lastErrorText);
}

void setupRuntimeIdentity() {
  deviceSerialNumber = "SN-" + getChipHexId();
  runtimeDeviceId = String(DEVICE_ID) + "-" + getChipHexId();
  bootResetReason = ESP.getResetReason();
}

