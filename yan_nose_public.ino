/*
 * ESP32-S3 电子鼻示例（BME688 / I2C）+ WiFi：每 10 秒把读数通过 HTTP POST 发给后端
 *
 * 需要的库（Arduino IDE 库管理器搜索安装）：
 *   - Adafruit BME680 Library
 *   - Adafruit Unified Sensor
 *   - Adafruit BusIO（上面两个库的依赖，一般会自动提示安装）
 *
 * 接线（BME688 走 I2C）：
 *   传感器 3V3 -> 开发板 3V3
 *   传感器 GND -> 开发板 GND
 *   传感器 SDA -> 开发板 GPIO8（示例引脚，请以你板子丝印/文档为准）
 *   传感器 SCL -> 开发板 GPIO9（示例引脚，请以你板子丝印/文档为准）
 *   注意：不同板子/模块的引脚编号不一样，务必对照自己板子的丝印核对，
 *   不要只看杜邦线的颜色接线，线色跟功能没有必然对应关系，容易接错。
 *   不要把传感器供电接到 5V。
 *
 * Arduino IDE 设置：
 *   - 开发板选择对应的 ESP32S3 Dev Module（先装好 Espressif 的 esp32 开发板包）
 *   - "USB CDC On Boot" 一定要选 Enabled，否则串口监视器可能看不到输出
 *   - 串口监视器波特率：115200
 *
 * 烧录小贴士：
 *   如果上传时一直卡住 / 烧不进去，先按住板子上的 BOOT 键不放，
 *   再按一下 RST 键，松开 RST，等上传开始后再松开 BOOT，进入下载模式后重试。
 *
 * 下面的 WiFi 名称/密码、后端地址、鉴权 token 都是占位符，
 * 换成你自己的实际信息即可直接使用。
 */
#define SDA_PIN 8
#define SCL_PIN 9

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* URL   = "https://your-backend.example.com/nose";
const char* TOKEN = "YOUR_TOKEN";

const uint32_t SEND_INTERVAL_MS = 10000;
uint32_t lastSendMs = 0;
uint32_t lastWifiTryMs = 0;
float lastT = NAN, lastH = NAN, lastP = NAN;
uint32_t lastGas = 0;
bool haveReading = false;

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiTryMs < 15000 && lastWifiTryMs != 0) return;
  lastWifiTryMs = millis();
  Serial.println("连 WiFi 中…");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void sendToYan() {
  if (!haveReading || WiFi.status() != WL_CONNECTED) return;
  if (!isfinite(lastT) || !isfinite(lastH) || !isfinite(lastP) || lastGas == 0) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, URL)) { Serial.println("发送失败：连不上地址"); return; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-doll-token", TOKEN);
  String body = String("{\"temp\":") + String(lastT, 2) + ",\"hum\":" + String(lastH, 2)
              + ",\"pres\":" + String(lastP, 2) + ",\"gas\":" + String(lastGas)
              + ",\"rssi\":" + String(WiFi.RSSI()) + "}";
  int code = http.POST(body);
  Serial.printf("发给言：%d\n", code);
  http.end();
}

Adafruit_BME680 bme(&Wire);

const uint32_t READ_INTERVAL_MS = 3000;
const uint32_t SCAN_INTERVAL_MS = 30000;
const uint16_t HEATER_DURATION_MS = 150;
const uint16_t SCAN_TEMPERATURES[] = {200, 250, 300, 350, 400};

bool sensorReady = false;
uint32_t lastRetryMs = 0;
uint32_t lastReadMs = 0;
uint32_t lastScanMs = 0;

void scanI2C() {
  Serial.println("I2C 扫描开始");
  uint8_t count = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    const uint8_t error = Wire.endTransmission();
    if (error == 0) {
      Serial.printf("发现 I2C 地址：0x%02X\n", address);
      ++count;
    }
  }
  if (count == 0) {
    Serial.println("未扫描到 I2C 设备");
  }
  Serial.println("I2C 扫描结束");
}

bool connectSensor() {
  const uint8_t addresses[] = {0x77, 0x76};
  for (uint8_t address : addresses) {
    if (!bme.begin(address)) {
      continue;
    }
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, HEATER_DURATION_MS);
    Serial.printf("鼻子已连接：0x%02X\n", address);
    lastReadMs = millis() - READ_INTERVAL_MS;  // 连接后立即读一次
    lastScanMs = millis();
    return true;
  }
  Serial.println("没找到鼻子：检查 SDA/SCL 是不是插反了");
  return false;
}

void printNumber(float value, uint8_t decimals) {
  // 非有限数用 JSON null 表示，不输出非法 JSON 的 NaN/Infinity。
  if (isfinite(value)) {
    Serial.print(value, decimals);
  } else {
    Serial.print("null");
  }
}

bool printReading() {
  if (!bme.performReading()) {
    Serial.println("读取失败：稍后重新连接鼻子");
    return false;
  }
  Serial.print("{\"t\":");
  printNumber(bme.temperature, 2);
  Serial.print(",\"h\":");
  printNumber(bme.humidity, 2);
  Serial.print(",\"p\":");
  printNumber(bme.pressure / 100.0F, 2);
  Serial.print(",\"gas\":");
  Serial.print(bme.gas_resistance);
  Serial.println("}");
  lastT = bme.temperature; lastH = bme.humidity; lastP = bme.pressure / 100.0F;
  lastGas = bme.gas_resistance; haveReading = true;
  return true;
}

bool printHeaterScan() {
  uint32_t values[5] = {};
  bool valid[5] = {};
  bool allValid = true;

  for (uint8_t i = 0; i < 5; ++i) {
    bme.setGasHeater(SCAN_TEMPERATURES[i], HEATER_DURATION_MS);
    // performReading 等待本次强制测量（包括 150ms 加热）完成。
    valid[i] = bme.performReading();
    if (valid[i]) {
      values[i] = bme.gas_resistance;
    } else {
      allValid = false;
    }
  }
  // 无论扫描是否成功，都恢复常规测量的加热设置。
  bme.setGasHeater(320, HEATER_DURATION_MS);

  Serial.print("{\"scan\":[");
  for (uint8_t i = 0; i < 5; ++i) {
    if (i != 0) Serial.print(',');
    if (valid[i]) {
      Serial.print(values[i]);
    } else {
      Serial.print("null");
    }
  }
  Serial.println("]}");
  if (!allValid) {
    Serial.println("加热扫描读取失败：稍后重新连接鼻子");
  }
  return allValid;
}

void setup() {
  Serial.begin(115200);
  // USB 串口未打开时最多等 2 秒，不永久卡在 setup。
  const uint32_t serialStart = millis();
  while (!Serial && millis() - serialStart < 2000) {
    delay(10);
  }

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(100);  // 总线故障时也能超时返回
  scanI2C();
  sensorReady = connectSensor();
  lastRetryMs = millis();
  WiFi.mode(WIFI_STA);
  ensureWifi();
}

void loop() {
  ensureWifi();
  if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis();
    sendToYan();
  }
  const uint32_t now = millis();
  if (!sensorReady) {
    if (now - lastRetryMs >= READ_INTERVAL_MS) {
      sensorReady = connectSensor();
      lastRetryMs = millis();
    }
    delay(5);
    return;
  }

  if (now - lastReadMs >= READ_INTERVAL_MS) {
    lastReadMs = now;
    sensorReady = printReading();
    if (!sensorReady) {
      lastRetryMs = millis();
      return;
    }
  }

  if (millis() - lastScanMs >= SCAN_INTERVAL_MS) {
    lastScanMs = millis();
    sensorReady = printHeaterScan();
    if (!sensorReady) lastRetryMs = millis();
  }
  delay(5);
}
