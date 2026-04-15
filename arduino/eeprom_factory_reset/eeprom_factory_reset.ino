#include <EEPROM.h>
#include <ESP8266WiFi.h>

// 与主项目保持一致，避免只清了一部分配置
constexpr size_t EEPROM_SIZE = 896;

void clearEepromAllZero() {
  EEPROM.begin(EEPROM_SIZE);
  for (size_t i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0x00);
  }
  EEPROM.commit();
  EEPROM.end();
}

void clearWiFiCredentials() {
  // 清除 SDK 保存的 Wi-Fi 信息
  WiFi.persistent(true);
  WiFi.disconnect(true);
  delay(200);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("=== EEPROM Factory Reset Tool ===");
  Serial.println("Step1: Clearing EEPROM...");
  clearEepromAllZero();

  Serial.println("Step2: Clearing Wi-Fi credentials...");
  clearWiFiCredentials();

  Serial.println("Done. Device will restart in 2 seconds.");
  delay(2000);
  ESP.restart();
}

void loop() {
  // 一次性工具，不需要循环逻辑
}

