#include <Wire.h>
#include <Adafruit_GFX.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "Adafruit_CCS811.h"
#include <Arduino_GFX_Library.h>
#include "FreeSans9pt7b.h"
#include <WiFiManager.h>
#include <Preferences.h>
#include <SensirionI2cScd4x.h> 

// Thư viện mạng phục vụ Web Server & OTA nâng cao
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>

// ====================== ĐỊNH NGHĨA PHIÊN BẢN FIMWARE ======================
#define FIRMWARE_VERSION    "1.2.1"

// ====================== CẤU HÌNH PHẦN CỨNG ======================
#define ERROR_LED       2 
#define RXD2            16
#define TXD2            17
#define CONFIG_BUTTON   25
#define BUZZER_PIN      26

#define TFT_SCL         18
#define TFT_SDA         23
#define TFT_RES         15
#define TFT_DC          27
#define TFT_CS          5

const char* url = "https://ijfgklxwtacolgevxred.supabase.co/rest/v1/rpc/upsert_iaq";
const char* apiKey = "sb_publishable_bc8gAWEKPCo5roEWMVCDAA_1eWtB2F-";

// ====================== KHỞI TẠO ĐỐI TƯỢNG ======================
Preferences prefs;
char roomId[6] = "0";
char roomName[50] = "Unknown";

SensirionI2cScd4x scd4x;
Adafruit_CCS811 ccs;
Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCL, TFT_SDA, -1);
Arduino_GFX *tft = new Arduino_ST7789(bus, TFT_RES, 1, true, 240, 280, 0, 20, 0, 20);

WiFiManager wm;
WiFiManagerParameter custom_room("room", "New Room ID", "", 5);
WiFiManagerParameter custom_room_name( "roomname", "Room Name", "",50);

WebServer server(80);
bool isServerRunning = false; // Biến cờ kiểm soát trạng thái Web Server nội bộ

// ====================== BỘ LỌC TÍN HIỆU KALMAN ======================
class SimpleKalmanFilter {
  public:
    SimpleKalmanFilter(float mea_e, float est_e, float q) { _err_measure = mea_e; _err_estimate = est_e; _q = q; }
    float updateEstimate(float mea) {
      float _kalman_gain = _err_estimate / (_err_estimate + _err_measure);
      float _current_estimate = _last_estimate + _kalman_gain * (mea - _last_estimate);
      _err_estimate = (1.0 - _kalman_gain) * _err_estimate + fabs(_last_estimate - _current_estimate) * _q;
      _last_estimate = _current_estimate;
      return _current_estimate;
    }
  private:
    float _err_measure, _err_estimate, _q, _last_estimate = 0;
};
SimpleKalmanFilter tempK(0.4, 0.4, 0.02), humK(1.0, 1.0, 0.03);

// ====================== BỘ LỌC TRUNG VỊ (MEDIAN FILTER) ======================
template<int SIZE>
class MedianFilter {
  public:
    float update(float input) {
      if (!_initialized) {
        for (int i = 0; i < SIZE; i++) _buffer[i] = input;
        _initialized = true;
      }
      _buffer[_index] = input;
      _index = (_index + 1) % SIZE;
      float temp[SIZE];
      for (int i = 0; i < SIZE; i++) temp[i] = _buffer[i];
      for (int i = 0; i < SIZE - 1; i++) {
        for (int j = i + 1; j < SIZE; j++) {
          if (temp[j] < temp[i]) {
            float t = temp[i];
            temp[i] = temp[j]; temp[j] = t;
          }
        }
      }
      return temp[SIZE / 2];
    }
  private:
    float _buffer[SIZE] = {0};
    int _index = 0;
    bool _initialized = false;
};
MedianFilter<5> pm25Median; MedianFilter<5> pm10Median; MedianFilter<5> pm1Median;
MedianFilter<5> tvocMedian; MedianFilter<5> eco2Median;

// ====================== BỘ LỌC EMA THÍCH NGHỊ (ADAPTIVE EMA) ======================
class AdaptiveEMA {
  public:
    AdaptiveEMA(float slowAlpha, float fastAlpha, float threshold) {
      _slowAlpha = slowAlpha; _fastAlpha = fastAlpha; _threshold = threshold;
    }
    float update(float input) {
      if (!_initialized) { _last = input; _initialized = true; return _last; }
      float diff = fabs(input - _last);
      float alpha = (diff > _threshold) ? _fastAlpha : _slowAlpha;
      _last = alpha * input + (1.0f - alpha) * _last;
      return _last;
    }
  private:
    float _slowAlpha, _fastAlpha, _threshold, _last = 0; bool _initialized = false;
};
AdaptiveEMA pm25EMA(0.08, 0.35, 12.0); AdaptiveEMA pm10EMA(0.08, 0.35, 15.0);
AdaptiveEMA pm1EMA (0.08, 0.35, 10.0);  AdaptiveEMA co2EMA (0.05, 0.25, 80.0);
AdaptiveEMA tvocEMA(0.02, 0.18, 80.0); AdaptiveEMA eco2EMA(0.03, 0.20, 100.0);

// ====================== SLEW RATE LIMITER CHO TFT UI ======================
class SlewRateLimiter {
  public:
    SlewRateLimiter(float maxStepPerSec) { _maxStep = maxStepPerSec; }
    float update(float target, float deltaSec) {
      if (!_initialized) { _current = target; _initialized = true; return _current; }
      float diff = target - _current;
      float step = _maxStep * deltaSec;
      if (fabs(diff) > step) { _current += (diff > 0) ? step : -step; } 
      else { _current = target; }
      return _current;
    }
  private:
    float _current = 0; float _maxStep; bool _initialized = false;
};
SlewRateLimiter co2Display(50.0); SlewRateLimiter pm25Display(2.0);

// ================= BITMAPS ĐỒ HỌA MÀN HÌNH =================
const unsigned char epd_bitmap_network_4_bars [] PROGMEM = { 0x00, 0x0e, 0x00, 0x0e, 0x00, 0x0e, 0x00, 0x0e, 0x00, 0xee, 0x00, 0xee, 0x00, 0xee, 0x00, 0xee, 0x0e, 0xee, 0x0e, 0xee, 0x0e, 0xee, 0x0e, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0x00, 0x00 };
const unsigned char epd_bitmap_wifi_not_connected [] PROGMEM = { 0x21, 0xf0, 0x00, 0x16, 0x0c, 0x00, 0x08, 0x03, 0x00, 0x25, 0xf0, 0x80, 0x42, 0x0c, 0x40, 0x89, 0x02, 0x20, 0x10, 0xa1, 0x00, 0x23, 0x58, 0x80, 0x04, 0x24, 0x00, 0x08, 0x52, 0x00, 0x01, 0xa8, 0x00, 0x02, 0x04, 0x00, 0x00, 0x42, 0x00, 0x00, 0xa1, 0x00, 0x00, 0x40, 0x80, 0x00, 0x00, 0x00 };
const unsigned char epd_bitmap_network_www [] PROGMEM = { 0x03, 0xc0, 0x0d, 0xb0, 0x32, 0x4c, 0x24, 0x24, 0x44, 0x22, 0x7f, 0xfe, 0x88, 0x11, 0x88, 0x11, 0x88, 0x11, 0x88, 0x11, 0x7f, 0xfe, 0x44, 0x22, 0x24, 0x24, 0x32, 0x4c, 0x0d, 0xb0, 0x03, 0xc0 };
const unsigned char epd_bitmap_wifi [] PROGMEM = { 0x01, 0xf0, 0x00, 0x06, 0x0c, 0x00, 0x18, 0x03, 0x00, 0x21, 0xf0, 0x80, 0x46, 0x0c, 0x40, 0x88, 0x02, 0x20, 0x10, 0xe1, 0x00, 0x23, 0x18, 0x80, 0x04, 0x04, 0x00, 0x08, 0x42, 0x00, 0x01, 0xb0, 0x00, 0x02, 0x08, 0x00, 0x00, 0x40, 0x00, 0x00, 0xa0, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00 };
const unsigned char epd_bitmap_weather_temperature [] PROGMEM = { 0x03, 0xf0, 0x00, 0x00, 0x03, 0xf0, 0x00, 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0xcf, 0x00, 0x33, 0x0c, 0xcf, 0x00, 0x33, 0x0c, 0xcc, 0x00, 0x0c, 0x0c, 0xcc, 0x00, 0x0c, 0x0c, 0xcf, 0x0f, 0xc0, 0x0c, 0xcf, 0x0f, 0xc0, 0x0c, 0xcc, 0x3c, 0x00, 0x0c, 0xcc, 0x3c, 0x00, 0x0c, 0xcf, 0x30, 0x00, 0x0c, 0xcf, 0x30, 0x00, 0x0c, 0xcc, 0x30, 0x00, 0x0c, 0xcc, 0x30, 0x00, 0x0c, 0xcc, 0x3c, 0x00, 0x0c, 0xcc, 0x3c, 0x00, 0x30, 0xc3, 0x0f, 0xc0, 0x30, 0xc3, 0x0f, 0xc0, 0xc3, 0xf0, 0xc0, 0x00, 0xc3, 0xf0, 0xc0, 0x00, 0xcc, 0xfc, 0xc0, 0x00, 0xcc, 0xfc, 0xc0, 0x00, 0xcf, 0xfc, 0xc0, 0x00, 0xcf, 0xfc, 0xc0, 0x00, 0xc3, 0xf0, 0xc0, 0x00, 0xc3, 0xf0, 0xc0, 0x00, 0x30, 0x03, 0x00, 0x00, 0x30, 0x03, 0x00, 0x00, 0x0f, 0xfc, 0x00, 0x00, 0x0f, 0xfc, 0x00, 0x00 };
const unsigned char epd_bitmap_weather_humidity [] PROGMEM = { 0x00, 0x30, 0x00, 0x00, 0x30, 0x00, 0x00, 0x30, 0x00, 0x00, 0x30, 0x00, 0x00, 0xf0, 0x00, 0x00, 0xf0, 0x00, 0x00, 0xfc, 0x00, 0x00, 0xfc, 0x00, 0x03, 0xfc, 0x00, 0x03, 0xfc, 0x00, 0x03, 0xff, 0x00, 0x03, 0xff, 0x00, 0x0f, 0xff, 0xc0, 0x0f, 0xff, 0xc0, 0x0f, 0xff, 0xc0, 0x0f, 0xff, 0xc0, 0x3f, 0xfc, 0xf0, 0x3f, 0xfc, 0xf0, 0x3f, 0xff, 0x30, 0x3f, 0xff, 0x30, 0xff, 0xff, 0x3c, 0xff, 0xff, 0x3c, 0xff, 0xff, 0xfc, 0xff, 0xff, 0xfc, 0x3f, 0xff, 0xf0, 0x3f, 0xff, 0xf0, 0x3f, 0xff, 0xf0, 0x3f, 0xff, 0xf0, 0x0f, 0xff, 0xc0, 0x0f, 0xff, 0xc0, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00 };

// ================= BIẾN CHIA SẺ VÀ CƠ CHẾ ĐỒNG BỘ FREERTOS =================
struct SharedData {
  float    sh_temp = 0.0; float    sh_hum = 0.0;
  uint16_t sh_pm25 = 0;   uint16_t sh_pm10 = 0;
  int      sh_aqi = 0;    int      sh_pm1 = 0;
  int      sh_co2 = 0;     int      sh_eco2 = 0; int      sh_tvoc = 0;
  bool     sh_isPushSuccess = false; bool     sh_isConnected = false;
  bool     sh_pmsStatus = false;     bool     sh_scdStatus = false; bool     sh_ccsStatus = false;
  bool     sh_portalRunning = false; bool     sh_otaRunning = false;    
  bool     sh_mustRedraw = false; 
};
SharedData shared; 
uint8_t pmsBuffer[32]; 

SemaphoreHandle_t xMutex = NULL; TaskHandle_t xTaskTFTHandle = NULL; TaskHandle_t portalTaskHandle = NULL;

unsigned long lastBuzzerToggle = 0; bool buzzerState = false; unsigned long bootTime = 0; uint8_t httpFailCount = 0;
unsigned long lastButtonChange = 0; bool lastButtonState = HIGH; bool stableButtonState = HIGH; unsigned long pressStart = 0; bool buttonHolding = false;

// ====================== CÁC HÀM TRỢ GIÚP ĐỒ HỌA MÀN HÌNH VÀ TÍNH TOÁN ======================
void drawStringCenter(const char* text, int x_box, int y_baseline, int w_box) {
  int16_t x1, y1; uint16_t w, h;
  tft->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  tft->setCursor(x_box + (w_box - w) / 2, y_baseline); tft->print(text);
}

uint16_t getAQIColor(int val) {
  if (val <= 50)  return 0x07E0;  if (val <= 100) return 0xFFE0;
  if (val <= 150) return 0xFBE0;  if (val <= 200) return 0xF800; 
  if (val <= 300) return 0x89ED;  return 0x7804;
}

void checkAlertsNonBlocking(int aqi, float pm25, int co2, float temp, bool isPortalActive) {
  static unsigned long cycleStart = 0;
  static bool lastCritical = false;
  if (isPortalActive || shared.sh_otaRunning) { digitalWrite(BUZZER_PIN, LOW); return; }
  bool isCritical = (aqi > 150) || (pm25 > 65) || (co2 > 1500) || (temp > 38.0);
  if (isCritical){
        unsigned long currentMillis = millis();
        if (!lastCritical) {
            cycleStart = currentMillis; lastCritical = true;
        } if (currentMillis - cycleStart > 13000) {
            cycleStart = currentMillis;
        }
        unsigned long t = currentMillis - cycleStart;
        if ((t < 300) || (t > 600 && t < 900) || (t > 1200 && t < 1500)) {
            digitalWrite(BUZZER_PIN, HIGH);
        } else {
            digitalWrite(BUZZER_PIN, LOW);
        }
    } else { digitalWrite(BUZZER_PIN, LOW); buzzerState = false; }
}

bool readPMS() {
  if (Serial2.available() < 32) return false;
  if (Serial2.peek() != 0x42) { Serial2.read(); return false; }
  uint8_t frame[32];
  if (Serial2.read() != 0x42) return false; if (Serial2.read() != 0x4D) return false;
  frame[0] = 0x42; frame[1] = 0x4D;
  if (Serial2.readBytes(&frame[2], 30) != 30) return false;
  uint16_t sum = 0;
  for (int i = 0; i < 30; i++) sum += frame[i];
  uint16_t checksum = ((uint16_t)frame[30] << 8) | frame[31];
  if (sum != checksum) return false;
  memcpy(pmsBuffer, frame, 32); return true;
}

// ====================== US EPA AQI (PM2.5 ONLY) ======================
int calcAQI_PM25_US(float pm25) {
    float Clow, Chigh;
    int Ilow, Ihigh;
    if (pm25 < 0) return 0;
    if (pm25 <= 12.0) { Clow = 0.0;   Chigh = 12.0;   Ilow = 0;     Ihigh = 50;
    } else if (pm25 <= 35.4) { Clow = 12.1;  Chigh = 35.4; Ilow = 51;    Ihigh = 100;
    } else if (pm25 <= 55.4) { Clow = 35.5;  Chigh = 55.4; Ilow = 101;   Ihigh = 150;
    } else if (pm25 <= 150.4) {  Clow = 55.5;  Chigh = 150.4; Ilow = 151;   Ihigh = 200;
    } else if (pm25 <= 250.4) { Clow = 150.5; Chigh = 250.4;  Ilow = 201;   Ihigh = 300;
    } else if (pm25 <= 350.4) {  Clow = 250.5; Chigh = 350.4;  Ilow = 301;   Ihigh = 400;
    } else if (pm25 <= 500.4) {  Clow = 350.5; Chigh = 500.4;  Ilow = 401;   Ihigh = 500;
    } else { return 500;
    }
    return round(
        ((float)(Ihigh - Ilow) / (Chigh - Clow)) * (pm25 - Clow) + Ilow
    );
}

void drawInterfaceStatic() {
  tft->fillScreen(0x0000); tft->setFont(&FreeSans9pt7b); tft->setTextColor(0xFFFF);
  tft->drawLine(196, 0, 196, 116, 0xFFFF); tft->drawLine(197, 116, 280, 116, 0xFFFF);
  tft->drawLine(196, 121, 196, 239, 0xFFFF); tft->drawLine(196, 121, 279, 121, 0xFFFF);
  tft->drawLine(84, 122, 84, 240, 0xFFFF); tft->drawLine(83, 122, 0, 122, 0xFFFF);
  tft->drawLine(84, 116, 84, 0, 0xFFFF); tft->drawLine(83, 116, 0, 116, 0xFFFF);
  tft->drawLine(87, 141, 191, 141, 0xFFFF); tft->drawLine(87, 141, 87, 240, 0xFFFF);
  tft->drawLine(192, 141, 192, 239, 0xFFFF); tft->drawLine(87, 116, 87, 0, 0xFFFF);
  tft->drawLine(88, 116, 192, 116, 0xFFFF); tft->drawLine(192, 116, 192, 0, 0xFFFF);
  tft->drawBitmap(203, 5, epd_bitmap_weather_temperature, 32, 32, 0x7F2);
  tft->drawBitmap(202, 131, epd_bitmap_weather_humidity, 22, 32, 0x7F2);
  tft->setTextSize(1); tft->setCursor(15, 25);  tft->print("PM2.5");
  tft->setCursor(15, 145); tft->print("TVOC"); tft->setCursor(254, 100);tft->print("C");
  tft->setCursor(254, 225);tft->print("%"); tft->drawCircle(248, 88, 2, 0xFFFF);
}

// ====================== CẬP NHẬT GIÁ TRỊ LÊN MÀN HÌNH TFT CỦA THIẾT BỊ ======================
void updateTFTValues(int local_aqi, int local_tvoc, int local_co2, float local_pm25, 
                     float local_temp, float local_hum, bool local_push, bool local_wifi,
                     bool pmsOk, bool scdOk, bool ccsOk, bool localPortalRunning, float deltaSec, bool mustRedraw) {
  tft->setFont(&FreeSans9pt7b);
  static int lastAQI = -1; static float lastPM25 = -1; static int lastCO2 = -1; static int lastTVOC = -1;
  static float lastTemp = -1.0; static float lastHum = -1.0; static bool lastPmsOk = true;
  static bool lastScdOkCO2  = true; static bool lastScdOkTemp = true; static bool lastScdOkHum  = true;
  static bool lastCcsOk = true; static bool lastPush = false; static bool lastWifi = false; static bool lastOtaRunning = false;
  char buf[16];

  if (mustRedraw) {
    drawInterfaceStatic(); 
    lastAQI = -1; lastPM25 = -1.0; lastCO2 = -1; lastTVOC = -1;
    lastTemp = -1.0; lastHum = -1.0;
    lastPush = !local_push; lastWifi = !local_wifi; 
  }

  int displayCO2  = (int)co2Display.update(local_co2, deltaSec);
  int displayPM25 = (int)pm25Display.update(local_pm25, deltaSec);

  if (shared.sh_otaRunning) {
    if (!lastOtaRunning) {
      tft->fillRect(88, 1, 104, 115, 0xF800); tft->setTextColor(0xFFFF); tft->setTextSize(1);
      drawStringCenter("OTA", 88, 45, 104); drawStringCenter("UPGRADE", 88, 85, 104);
      lastOtaRunning = true;
    }
    return; 
  }
  lastOtaRunning = false;

  if (localPortalRunning) {
    tft->fillRect(88, 1, 104, 115, 0x001F); tft->setTextColor(0xFFFF); tft->setTextSize(1);
    drawStringCenter("WIFI", 88, 45, 104); drawStringCenter("CONFIG", 88, 85, 104); 
    lastAQI = -1; 
  } else if (local_aqi != lastAQI || pmsOk != lastPmsOk) {
    uint16_t aqiColor = pmsOk ? getAQIColor(local_aqi) : 0xF800; 
    tft->fillRect(88, 1, 104, 115, aqiColor); tft->setTextColor(aqiColor == 0xFFE0 ? 0x0000 : 0xFFFF);
    tft->setTextSize(2); drawStringCenter("AQI", 88, 35, 104); tft->setTextSize(pmsOk ? 3 : 2);
    drawStringCenter(pmsOk ? itoa(local_aqi, buf, 10) : "ERR", 88, 95, 104); 
    lastAQI = local_aqi; lastPmsOk = pmsOk;
  }

  if (local_push != lastPush || local_wifi != lastWifi) {
    tft->fillRect(85, 117, 106, 23, 0x0000);
    if (local_push) tft->drawBitmap(115, 122, epd_bitmap_network_4_bars, 15, 16, 0x07E0);
    else tft->drawBitmap(90, 122, epd_bitmap_network_www, 16, 16, 0xFFFF);
    tft->drawBitmap(local_wifi ? 170 : 145, 122, local_wifi ? epd_bitmap_wifi : epd_bitmap_wifi_not_connected, 19, 16, local_wifi ? 0x07E0 : 0xF800);
    lastPush = local_push; lastWifi = local_wifi;
  }

  if (displayCO2 != lastCO2 || scdOk != lastScdOkCO2) {
    tft->fillRect(95, 170, 90, 40, 0x0000); tft->setTextColor(0xFFFF); tft->setTextSize(1); drawStringCenter("CO2", 87, 160, 105);   
    if (scdOk) {
      uint16_t co2Color = (displayCO2 <= 800 ? 0x0720 : (displayCO2 <= 1200 ? 0xFFE0 : (displayCO2 <= 2000 ? 0xFBE0 : 0xF800)));
      tft->setTextColor(co2Color); tft->setTextSize(2); itoa(displayCO2, buf, 10);
    } else { tft->setTextColor(0x7BEF); tft->setTextSize(2); strcpy(buf, "ERR"); }
    drawStringCenter(buf, 87, 200, 105); tft->setTextSize(1); tft->setTextColor(0xFFFF); drawStringCenter("ppm", 87, 225, 105); 
    lastCO2 = displayCO2; lastScdOkCO2 = scdOk;
  }

  if (displayPM25 != (int)lastPM25 || pmsOk != lastPmsOk) {
    tft->fillRect(5, 40, 75, 40, 0x0000);
    if (pmsOk) {
      uint16_t pmColor = (displayPM25 <= 12 ? 0x0720 : (displayPM25 <= 35 ? 0xFFE0 : (displayPM25 <= 55 ? 0xFBE0 : (displayPM25 <= 150 ? 0xF800 : (displayPM25 <= 250 ? 0x89ED : 0x7804)))));
      tft->setTextColor(pmColor); tft->setTextSize(2); itoa(displayPM25, buf, 10);
    } else { tft->setTextColor(0x7BEF); tft->setTextSize(2); strcpy(buf, "ERR"); }
    drawStringCenter(buf, 0, 75, 84); tft->setTextSize(1); tft->setTextColor(0xFFFF); drawStringCenter("ug/m3", 0, 105, 84); 
    lastPM25 = displayPM25;
  }

  if (local_tvoc != lastTVOC || ccsOk != lastCcsOk) {
    tft->fillRect(-1, 160, 85, 40, 0x0000);
    if (ccsOk) {
      uint16_t tvocColor = (local_tvoc <= 300 ? 0x0720 : (local_tvoc <= 500 ? 0xFFE0 : (local_tvoc <= 1000 ? 0xFBE0 : (local_tvoc <= 3000 ? 0xF800 : 0x89ED))));
      tft->setTextColor(tvocColor); tft->setTextSize(2); itoa(local_tvoc, buf, 10);
    } else { tft->setTextColor(0x7BEF); tft->setTextSize(2); strcpy(buf, "ERR"); }
    drawStringCenter(buf, -3, 195, 84); tft->setTextSize(1); tft->setTextColor(0xFFFF); drawStringCenter("ppb", 0, 225, 84); 
    lastTVOC = local_tvoc; lastCcsOk = ccsOk;
  }

  if (abs(local_temp - lastTemp) >= 0.1 || scdOk != lastScdOkTemp) {
    tft->fillRect(198, 45, 82, 35, 0x0000);
    if (scdOk) {
      uint16_t tempColor = (local_temp <= 16.0 ? 0x07E0 : (local_temp <= 20.0 ? 0x92BF : (local_temp <= 25.0 ? 0x0720 : (local_temp <= 30.0 ? 0xFFE0 : (local_temp <= 35.0 ? 0xFBE0 : 0xF800)))));
      tft->setTextColor(tempColor); tft->setTextSize(2); dtostrf(local_temp, 1, 1, buf);
    } else { tft->setTextColor(0x7BEF); tft->setTextSize(2); strcpy(buf, "ERR"); }
    drawStringCenter(buf, 196, 75, 84); lastTemp = local_temp; lastScdOkTemp = scdOk;
  }

  if (abs((int)local_hum - (int)lastHum) >= 1 || scdOk != lastScdOkHum) {
    tft->fillRect(198, 166, 82, 35, 0x0000);
    if (scdOk) {
      uint16_t humColor = (local_hum <= 25 ? 0xFE4C : (local_hum <= 40 ? 0xFFE0 : (local_hum <= 60 ? 0x0720 : (local_hum <= 75 ? 0x665F : 0x53DF))));
      tft->setTextColor(humColor); tft->setTextSize(2); itoa((int)local_hum, buf, 10);
    } else { tft->setTextColor(0x7BEF); tft->setTextSize(2); strcpy(buf, "ERR"); }
    drawStringCenter(buf, 196, 195, 84); lastHum = local_hum; lastScdOkHum = scdOk;
  }
}

void saveWifiCallback(){
    String newRoom = custom_room.getValue();
    if (newRoom.length() > 0) {  prefs.putString("room_id", newRoom); newRoom.toCharArray(roomId, sizeof(roomId));
    }
    String newRoomName = custom_room_name.getValue();
    if (newRoomName.length() > 0){  prefs.putString( "room_name", newRoomName); newRoomName.toCharArray( roomName, sizeof(roomName));
    }
    tft->fillRect(88,1,104,115,0x0400); drawStringCenter("SAVED",88,45,104); delay(1500); ESP.restart();
}

void onOTAStart() { shared.sh_otaRunning = true; digitalWrite(BUZZER_PIN, LOW); }
void onOTAEnd(bool success) { shared.sh_otaRunning = false; }

// ====================== PORTAL CONFIG TASK (ĐƯỢC GỌI KHI NHẤN GIỮ NÚT) ======================
void PortalTask(void *pvParameters) {
  if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) { 
    shared.sh_portalRunning = true; 
    xSemaphoreGive(xMutex); 
  }
  digitalWrite(BUZZER_PIN, LOW); 
  
  // Tắt hoàn toàn Web Server nội bộ để giải phóng bộ nhớ và tránh lỗi tranh chấp cổng mạng (Port 80)
  server.stop(); 
  isServerRunning = false;
  WiFi.mode(WIFI_AP_STA);
  
  // Kích hoạt chế độ giao diện tối hệ thống cho TẤT CẢ các trang con (/wifi, /info)
  wm.setDarkMode(true);
  wm.setClass("invert"); 

  // Cấu hình các tham số cơ bản cho WiFiManager
  wm.setSaveConfigCallback(saveWifiCallback); 
  wm.setConfigPortalTimeout(90); 
  wm.setConnectTimeout(20); 
  wm.setBreakAfterConfig(true); 
  wm.setShowInfoErase(false); 
  wm.setShowInfoUpdate(false); 
  
  // Cấu hình Menu: Giữ cấu hình (wifi), thông tin (info), thoát (exit). Bỏ hoàn toàn Update.
    const char* menu[] = {"wifi", "info", "update", "exit"};
  wm.setMenu(menu, 4);

  // Ép CSS bao phủ toàn cục - Giữ nguyên vạch sóng chuẩn vị trí bên phải
  wm.setCustomHeadElement(R"rawliteral(
<style>
body, body.invert{background:#18191A!important;color:#E4E6EB!important;font-family:sans-serif!important;}
.wrap, body.invert .wrap{background:#242526!important;max-width:420px!important;padding:35px 25px!important;border-radius:16px!important;box-shadow:0 12px 40px rgba(0,0,0,.5)!important;border:none!important;}

/* Triệt tiêu hoàn toàn các ô nền đen tương phản */
body.invert div, body.invert p, body.invert label, body.invert h1, body.invert h3, body.invert a {
  background: transparent !important;
  background-color: transparent !important;
  color:#E4E6EB!important;
}

/* Định dạng cụm tiêu đề chính của trạm giám sát IAQ SYSTEM */
h1{font-size:0!important;padding:0!important;margin:0 0 25px 0!important;text-align:center!important;}
h1::before{content:'IAQ MONITOR SYSTEM'!important;display:block!important;color:#F5F6F7!important;font-size:24px!important;font-weight:700!important;letter-spacing:1px!important;line-height:1.2!important;}
h1::after{content:'Hệ thống giám sát chất lượng không khí'!important;display:block!important;color:#90949C!important;font-size:13px!important;font-weight:normal!important;margin-top:6px!important;}
h3{display:none!important;}

/* ===== WIFI LIST ===== */

.wrap > div > div{
    display:flex !important;
    align-items:center !important;
}

.wrap div[role='img']{
    order:1 !important;
    width:24px !important;
    margin-right:12px !important;
    position:static !important;
}

.wrap a[href^='#p']{
    order:2 !important;
    flex:1 !important;
}

.wrap div.h{
    order:3 !important;
    width:50px !important;
    text-align:right !important;
    position:static !important;
}

/* Định dạng hộp thông tin phòng hiện tại */
.room-box{background:#1D1E1F!important;padding:14px!important;border-radius:10px!important;margin-top:15px!important;margin-bottom:20px!important;text-align:left!important;font-size:14px!important;border-bottom:1px solid #3A3B3C!important;line-height:1.6!important;}
.room-box b{color:#10B981!important;float:right!important;}

/* Định dạng hệ thống nút bấm hành động (Save, Info, Exit) */
button, input[type='submit'], .wrap a:not([href^='#p']) {
  display:block!important;width:100%!important;background:transparent!important;color:#10B981!important;border:1px solid #10B981!important;padding:12px 0!important;border-radius:10px!important;font-weight:600!important;text-decoration:none!important;text-align:center!important;margin-bottom:10px!important;box-sizing:border-box!important;
}
button:hover, input[type='submit']:hover, .wrap a:not([href^='#p']):hover{
  background:#10B981!important;color:#18191A!important;box-shadow:0 8px 20px rgba(16,185,129,0.2)!important;
}

/* Kiểu dáng các ô TextBox nhập liệu form tiêu chuẩn */
body.invert input[type='text'], body.invert input[type='password'], select{
  background:#1D1E1F!important;border:1px solid #3A3B3C!important;border-radius:8px!important;padding:12px!important;color:#E4E6EB!important;width:100%!important;box-sizing:border-box!important;margin-top:5px!important;margin-bottom:12px!important;
}
input:focus, select:focus{border-color:#10B981!important;outline:none!important;}
.msg{color:#90949C!important;font-size:13px!important;text-align:center!important;}
</style>
)rawliteral");

  // 1. Chỉ chèn hộp thông tin hiển thị trạng thái phòng hiện tại vào BODY trang web
  String htmlContent = "<div class='room-box'>Mã phòng hiện tại: <b>" + String(roomId) + "</b><br>Tên phòng hiện tại: <b>" + String(roomName) + "</b></div>";
  WiFiManagerParameter custom_html(htmlContent.c_str());
  wm.addParameter(&custom_html);
  
  // 2. Không gọi wm.addParameter(&custom_room) ở đây để dứt điểm hoàn toàn lỗi lặp ô nhập liệu
  custom_room.setValue(roomId, 5);

  yield(); 
  delay(200);
 
  // Khởi chạy cổng cấu hình an toàn chặn luồng
  wm.startConfigPortal("IAQ-SETUP", "admin123");
  
  // Khi thoát khỏi Config Portal
  if(xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE){
    shared.sh_portalRunning = false;
    shared.sh_mustRedraw = true; 
    xSemaphoreGive(xMutex);
  }
  
  WiFi.mode(WIFI_STA);            
  if(xTaskTFTHandle){ xTaskNotifyGive(xTaskTFTHandle); } 
  portalTaskHandle = NULL; 
  vTaskDelete(NULL);           
}

// ====================== TASK ĐỌC CẢM BIẾN (CORE 1) ======================
void TaskSensors(void *pvParameters) {
  unsigned long lastScdRead = 0; unsigned long lastCcsRead = 0; unsigned long lastPmsPacketTime = 0; unsigned long lastScdSuccess = 0;
  while(1) {
    if (shared.sh_otaRunning) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    bool hasNewData = false; unsigned long currentMillis = millis();
    
    if (readPMS()) {
      lastPmsPacketTime = currentMillis;
      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(15)) == pdTRUE) {
        shared.sh_pmsStatus = true;
        uint16_t pm1_raw  = ((uint16_t)pmsBuffer[10] << 8) | pmsBuffer[11];
        uint16_t pm25_raw = ((uint16_t)pmsBuffer[12] << 8) | pmsBuffer[13];
        uint16_t pm10_raw = ((uint16_t)pmsBuffer[14] << 8) | pmsBuffer[15];
        float pm1Med = pm1Median.update(pm1_raw); float pm25Med = pm25Median.update(pm25_raw); float pm10Med = pm10Median.update(pm10_raw);
        shared.sh_pm1   = (int)pm1EMA.update(pm1Med); shared.sh_pm25  = (int)pm25EMA.update(pm25Med); shared.sh_pm10  = (int)pm10EMA.update(pm10Med);  
        shared.sh_aqi = calcAQI_PM25_US(shared.sh_pm25);
        xSemaphoreGive(xMutex); hasNewData = true;
      }
    }

    if (currentMillis - lastPmsPacketTime > 3500) {
      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) { if(shared.sh_pmsStatus) { shared.sh_pmsStatus = false; hasNewData = true; } xSemaphoreGive(xMutex); }
    }

    if (currentMillis - lastCcsRead >= 1000) {
      lastCcsRead = currentMillis; int local_tvoc = 0; int local_eco2 = 0; bool current_ccs_status = false; float env_t = 25.0; float env_h = 50.0;
      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) { env_t = shared.sh_temp; env_h = shared.sh_hum; xSemaphoreGive(xMutex); }
      if (ccs.available()) {
        ccs.setEnvironmentalData(env_h, env_t);
        if (!ccs.readData()) {
          float tvocMed = tvocMedian.update(ccs.getTVOC()); float eco2Med = eco2Median.update(ccs.geteCO2());
          local_tvoc = (int)tvocEMA.update(tvocMed); local_eco2 = (int)eco2EMA.update(eco2Med); current_ccs_status = true;
        }
      }
      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(15)) == pdTRUE) {
        shared.sh_ccsStatus = current_ccs_status; if (current_ccs_status) { shared.sh_tvoc = local_tvoc; shared.sh_eco2 = local_eco2; }
        xSemaphoreGive(xMutex); hasNewData = true;
      }
    }

    if (currentMillis - lastScdRead >= 5000) {
      lastScdRead = currentMillis; uint16_t co2 = 0; float temperature = 0; float humidity = 0; bool current_scd_status; bool isDataReady = false; float local_t = 0; float local_h = 0; int local_co2 = 0;
      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) { local_t = shared.sh_temp; local_h = shared.sh_hum; local_co2 = shared.sh_co2; current_scd_status = shared.sh_scdStatus; xSemaphoreGive(xMutex); }
      uint16_t error = scd4x.getDataReadyStatus(isDataReady);
      if (!error && isDataReady) {
        error = scd4x.readMeasurement(co2, temperature, humidity);
        if (!error && co2 > 350) { local_t = tempK.updateEstimate(temperature - 2.0); local_h = humK.updateEstimate(humidity); local_co2 = (int)co2EMA.update(co2); current_scd_status = true; lastScdSuccess = currentMillis; }
      }
      if (currentMillis - lastScdSuccess > 15000) current_scd_status = false;
      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(15)) == pdTRUE) { shared.sh_scdStatus = current_scd_status; shared.sh_temp = local_t; shared.sh_hum = local_h; shared.sh_co2 = local_co2; xSemaphoreGive(xMutex); hasNewData = true; }
      if (!current_scd_status) { digitalWrite(ERROR_LED, HIGH); vTaskDelay(pdMS_TO_TICKS(50)); digitalWrite(ERROR_LED, LOW); }
    }
    if (hasNewData && (xTaskTFTHandle != NULL)) { xTaskNotifyGive(xTaskTFTHandle); }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ====================== TASK MẠNG & ĐỒ HỌA + CUSTOM OTA UI (CORE 0) ======================
void TaskNetworkAndTFT(void *pvParameters) {
  char jsonBuffer[512]; char aqiStr[8], tempStr[8], humStr[8], pm25Str[8], pm10Str[8], pm1Str[8], tvocStr[8], eco2Str[8], co2Str[8], statusStr[64];
  unsigned long lastReconnectAttempt = 0; unsigned long lastSupabasePush = 0; unsigned long lastUiRefreshTime = millis(); bool isFirstConnectDone = false;
  WiFiClientSecure secureClient;
  
  // Khởi tạo các Router định tuyến tĩnh cho Server Web tĩnh một lần duy nhất
  server.on("/", [&]() {
      String webPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>IAQ Monitor | Room )rawliteral";
      webPage += roomId;
      webPage += R"rawliteral(</title>
<style>
body{background:#18191A;color:#E4E6EB;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;padding:20px;margin:0;display:flex;justify-content:center;align-items:center;min-height:100vh;box-sizing:border-box;}
.card{background:#242526;width:100%;max-width:420px;padding:45px 30px;border-radius:16px;box-shadow:0 12px 40px rgba(0,0,0,0.5);box-sizing:border-box;text-align:center;}
.logo-container{display:flex;flex-direction:row;align-items:center;justify-content:center;margin-bottom:25px;}
.logo-text{font-size:36px;font-weight:800;letter-spacing:4px;line-height:1;}
h2{color:#F5F6F7;margin:0 0 6px 0;font-size:20px;letter-spacing:1px;font-weight:600;}
.sub{color:#90949C;font-size:13px;margin-bottom:35px;letter-spacing:0.5px;}
table{width:100%;margin:0 0 35px 0;border-collapse:collapse;}
td{padding:16px 0;font-size:14px;border-bottom:1px solid #3A3B3C;}
tr:last-child td{border-bottom:none;}
td.label{color:#90949C;font-weight:500;text-align:left;}
td.val{font-weight:600;color:#E4E6EB;text-align:right;}
.status-dot{display:inline-block;width:8px;height:8px;background:#22C55E;border-radius:50%;margin-right:6px;vertical-align:middle;}
.btn{display:block;background:transparent;color:#10B981;border:1px solid #10B981;text-decoration:none;padding:14px;border-radius:10px;font-weight:600;font-size:14px;letter-spacing:0.5px;transition:all 0.2s ease;max-width:280px;margin:0 auto;}
.btn:hover{background:#10B981;color:#18191A;box-shadow:0 8px 20px rgba(16,185,129,0.2);transform:translateY(-1px);}
</style>
</head>
<body>
<div class='card'>
  <div class='logo-container logo-text'>
  </div>
  
  <h2>IAQ MONITOR SYSTEM</h2>
  <div class='sub'>Hệ thống giám sát chất lượng không khí</div>
  
  <table>
    <tr><td class='label'>Room ID</td><td class='val'>)rawliteral";
      webPage += roomId;
      webPage += R"rawliteral(</td></tr>
    <tr><td class='label'>Room Name</td><td class='val'>)rawliteral";
      webPage += roomName;
      webPage += R"rawliteral(</td></tr>
    <tr><td class='label'>Firmware</td><td class='val'>v)rawliteral";
      webPage += FIRMWARE_VERSION;
      webPage += R"rawliteral(</td></tr>
    <tr><td class='label'>Status</td><td class='val' style='color:#22C55E;'><span class='status-dot'></span>Online</td></tr>
  </table>
  
  <a href='/update' class='btn'>UPDATE FIRMWARE OTA</a>
</div> </body>
</html>
)rawliteral";
      server.send(200, "text/html", webPage);
  });

  ElegantOTA.begin(&server);
  ElegantOTA.setAuth("admin", "admin123"); 
  ElegantOTA.onStart(onOTAStart); ElegantOTA.onEnd(onOTAEnd);
  ElegantOTA.setAutoReboot(true);

  while(1) {
    bool localPortalRunning = false;
    bool localMustRedraw = false;
    bool isWifiConnectedNow = (WiFi.status() == WL_CONNECTED);

    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        localPortalRunning = shared.sh_portalRunning;
        localMustRedraw = shared.sh_mustRedraw;
        if (localMustRedraw) shared.sh_mustRedraw = false; 
        xSemaphoreGive(xMutex);
    }
    
    // Khởi tạo mạng ban đầu không chặn (Non-blocking)
    if (!isFirstConnectDone) {
        WiFi.mode(WIFI_STA); WiFi.begin();
        unsigned long startAttempt = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 4000) { vTaskDelay(pdMS_TO_TICKS(100)); }
        drawInterfaceStatic(); isFirstConnectDone = true;
    }

    // Chỉ chạy Web Server khi đã kết nối WiFi thành công và Portal KHÔNG chạy
    if (isWifiConnectedNow && !localPortalRunning) {
        if (!isServerRunning) {
            char hostName[32]; snprintf(hostName, sizeof(hostName), "iaq-%s", roomId); 
            MDNS.begin(hostName); server.begin(); MDNS.addService("http", "tcp", 80);
            isServerRunning = true;
        }
        server.handleClient(); ElegantOTA.loop();
    }

    unsigned long currentUiTime = millis();
    float deltaSecUi = (currentUiTime - lastUiRefreshTime) / 1000.0;
    lastUiRefreshTime = currentUiTime; if (deltaSecUi <= 0.0) deltaSecUi = 0.05; 

    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(15)) == pdTRUE) { shared.sh_isConnected = isWifiConnectedNow; xSemaphoreGive(xMutex); }
    if (!isWifiConnectedNow && !localPortalRunning && (millis() - lastReconnectAttempt > 20000)) { lastReconnectAttempt = millis(); WiFi.begin(); }

    // XỬ LÝ NÚT BẤM ĐỂ VÀO PORTAL
    bool reading = digitalRead(CONFIG_BUTTON);
    if (reading != lastButtonState) { lastButtonChange = millis(); }
    if ((millis() - lastButtonChange) > 40) {
      if (reading != stableButtonState) { stableButtonState = reading; if (stableButtonState == LOW) { pressStart = millis(); buttonHolding = true; } else { buttonHolding = false; } }
    }
    lastButtonState = reading;
    if (buttonHolding) {
      if (millis() - pressStart > 5000 && !localPortalRunning) { 
        buttonHolding = false; 
        if (portalTaskHandle == NULL) { 
          xTaskCreatePinnedToCore(PortalTask, "PortalTask", 6144, NULL, 1, &portalTaskHandle, 1); 
        } 
      }
    }

    int local_aqi = 0, local_pm1 = 0, local_tvoc = 0, local_eco2 = 0, local_co2 = 0; float local_temp = 0, local_hum = 0, local_pm25 = 0, local_pm10 = 0;
    bool pmsOk = false, scdOk = false, ccsOk = false, local_push = false, local_wifi = false;
    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      local_aqi   = shared.sh_aqi; local_pm1   = shared.sh_pm1; local_tvoc  = shared.sh_tvoc; local_eco2  = shared.sh_eco2; local_co2 = shared.sh_co2; 
      local_temp  = shared.sh_temp; local_hum   = shared.sh_hum; local_pm25  = shared.sh_pm25; local_pm10  = shared.sh_pm10;
      pmsOk       = shared.sh_pmsStatus; scdOk = shared.sh_scdStatus; ccsOk = shared.sh_ccsStatus; local_push  = shared.sh_isPushSuccess; local_wifi = shared.sh_isConnected;
      xSemaphoreGive(xMutex);
    }

    checkAlertsNonBlocking(local_aqi, local_pm25, local_co2, local_temp, localPortalRunning);
    updateTFTValues(local_aqi, local_tvoc, local_co2, local_pm25, local_temp, local_hum, local_push, local_wifi, pmsOk, scdOk, ccsOk, localPortalRunning, deltaSecUi, localMustRedraw);
    
    if ((millis() - lastSupabasePush >= 10000) && !shared.sh_otaRunning && !localPortalRunning) {
      lastSupabasePush = millis();
      if (pmsOk) { itoa(local_aqi, aqiStr, 10); itoa(local_pm1, pm1Str, 10); snprintf(pm25Str, sizeof(pm25Str), "%d", (int)round(local_pm25)); snprintf(pm10Str, sizeof(pm10Str), "%d", (int)round(local_pm10)); }
      else { strcpy(aqiStr, "null"); strcpy(pm1Str, "null"); strcpy(pm25Str, "null"); strcpy(pm10Str, "null"); }
      if (scdOk) { dtostrf(local_temp, 1, 2, tempStr); dtostrf(local_hum, 1, 2, humStr); itoa(local_co2, co2Str, 10); } 
      else { strcpy(tempStr, "null"); strcpy(humStr, "null"); strcpy(co2Str, "null");}
      if (ccsOk) { itoa(local_tvoc, tvocStr, 10); itoa(local_eco2, eco2Str, 10); } else { strcpy(tvocStr, "null"); }
      strncpy(statusStr, roomName, sizeof(statusStr) - 1);
      statusStr[sizeof(statusStr) - 1] = '\0';
      
      snprintf(jsonBuffer, sizeof(jsonBuffer), "{\"p_room_id\":%s,\"p_aqi\":%s,\"p_temperature\":%s,\"p_humidity\":%s,\"p_pm2_5\":%s,\"p_pm10\":%s,\"p_pm1\":%s,\"p_co2\":%s,\"p_tvoc\":%s,\"p_eco2\":%s,\"p_status\":\"%s\"}", roomId, aqiStr, tempStr, humStr, pm25Str, pm10Str, pm1Str, co2Str, tvocStr, eco2Str, statusStr);
      bool targetPushResult = false;
      if (isWifiConnectedNow) {
        HTTPClient http; secureClient.setInsecure(); http.begin(secureClient, url); http.setTimeout(2500);
        http.addHeader("Content-Type", "application/json"); http.addHeader("apikey", apiKey);
        char authHeader[128]; snprintf(authHeader, sizeof(authHeader), "Bearer %s", apiKey); http.addHeader("Authorization", authHeader);
        
        unsigned long postStart = millis();
        int httpCode = http.POST(jsonBuffer); 
        unsigned long httpLatency = millis() - postStart;
        Serial.printf("[HTTP] Code=%d | Latency=%lu ms\n",
              httpCode,
              httpLatency);
        
        http.end();
        if (httpCode > 0) { httpFailCount = 0; targetPushResult = (httpCode == 204 || httpCode == 200 || httpCode == 201); } 
        else { httpFailCount++; if (httpFailCount >= 12) ESP.restart(); }
      }
      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(15)) == pdTRUE) { shared.sh_isPushSuccess = targetPushResult; xSemaphoreGive(xMutex); }
    }

    if (millis() - bootTime > 86400000UL) { delay(1000); ESP.restart(); }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ====================== SETUP THIẾT BỊ ======================
void setup() {
  Serial.begin(115200); Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2); Serial2.setTimeout(100); Serial2.setRxBufferSize(256);
  pinMode(ERROR_LED, OUTPUT); pinMode(CONFIG_BUTTON, INPUT_PULLUP); pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(21, 22); scd4x.begin(Wire, 0x62); scd4x.stopPeriodicMeasurement(); delay(300);
  scd4x.setAutomaticSelfCalibrationEnabled(1); scd4x.startPeriodicMeasurement(); Wire.setTimeout(50);
  tft->begin(); tft->setRotation(1); tft->fillScreen(0x0000); ccs.begin();

  prefs.begin("iaq-config", false);
  String savedRoom = prefs.getString("room_id", "0"); savedRoom.toCharArray(roomId, sizeof(roomId));
  String savedRoomName =prefs.getString("room_name", "Unknown"); savedRoomName.toCharArray(roomName,sizeof(roomName));
  // Khởi tạo giá trị ban đầu và đăng ký tham số ô nhập vào WiFiManager ngay khi khởi động
  custom_room.setValue(roomId, 5);
  custom_room_name.setValue(roomName, 50);

  wm.addParameter(&custom_room);
  wm.addParameter(&custom_room_name);

  bootTime = millis(); xMutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(TaskNetworkAndTFT, "TaskNetTFT", 12288, NULL, 1, &xTaskTFTHandle, 0);
  xTaskCreatePinnedToCore(TaskSensors, "TaskSensors", 4096, NULL, 2, NULL, 1);
}

void loop() { 
  vTaskDelete(NULL); 
}

