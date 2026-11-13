#define DEBUG 0

#if DEBUG
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(...)
#endif

// Realtime Airplane Radar UI - STEP 4
// CrowPanel 7.0" ESP32-S3 800x480 RGB
// Real aircraft data + background API update + sweep reveal effect
// by micemk May, 2026
#include <Arduino.h>
#include <Wire.h>
#include "secrets.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <driver/i2c.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include <math.h>
#include "time.h"


bool resolveADSBServer();


 // Wi-Fi credentials are stored in include/secrets.h (not committed to source control).


 // GLOBAL VARIABLES to enable one-off DNS lookup
IPAddress adsbServerIP;
bool adsbServerIPValid = false;

unsigned long adsbDnsResolvedAt = 0;
//const unsigned long ADSB_DNS_REFRESH_MS = 30UL * 60UL * 1000UL;
const unsigned long ADSB_DNS_REFRESH_MS = 30L * 60UL * 1000UL;

// Center location
const double HOME_LAT = 51.7563;  // for Ohrid
const double HOME_LON = 0.5077;  // for Ohrid

// Radar range
//const float RADAR_RANGE_KM = 100.0;
//const int API_RADIUS_NM = 54;   // about 100 km
const float RADAR_RANGE_KM = 50.0;
const int API_RADIUS_NM = 20;   // approximately 100 km

class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_RGB _bus_instance;
  lgfx::Panel_RGB _panel_instance;
  lgfx::Touch_GT911 _touch_instance;

  LGFX(void) {
    {
      auto cfg = _panel_instance.config();
      cfg.memory_width = 800;
      cfg.memory_height = 480;
      cfg.panel_width = 800;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel_instance.config(cfg);
    }

    {
      auto cfg = _panel_instance.config_detail();
      cfg.use_psram = 1;
      _panel_instance.config_detail(cfg);
    }

    {
      auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;

      cfg.pin_d0  = GPIO_NUM_21;  // B0
      cfg.pin_d1  = GPIO_NUM_47;  // B1
      cfg.pin_d2  = GPIO_NUM_48;  // B2
      cfg.pin_d3  = GPIO_NUM_45;  // B3
      cfg.pin_d4  = GPIO_NUM_38;  // B4
      cfg.pin_d5  = GPIO_NUM_9;   // G0
      cfg.pin_d6  = GPIO_NUM_10;  // G1
      cfg.pin_d7  = GPIO_NUM_11;  // G2
      cfg.pin_d8  = GPIO_NUM_12;  // G3
      cfg.pin_d9  = GPIO_NUM_13;  // G4
      cfg.pin_d10 = GPIO_NUM_14;  // G5
      cfg.pin_d11 = GPIO_NUM_7;   // R0
      cfg.pin_d12 = GPIO_NUM_17;  // R1
      cfg.pin_d13 = GPIO_NUM_18;  // R2
      cfg.pin_d14 = GPIO_NUM_3;   // R3
      cfg.pin_d15 = GPIO_NUM_46;  // R4

      cfg.pin_henable = GPIO_NUM_42;
      cfg.pin_vsync   = GPIO_NUM_41;
      cfg.pin_hsync   = GPIO_NUM_40;
      cfg.pin_pclk    = GPIO_NUM_39;
      cfg.freq_write = 16000000;

      cfg.hsync_polarity = 0;
      cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4;
      cfg.hsync_back_porch = 8;
      cfg.vsync_polarity = 0;
      cfg.vsync_front_porch = 8;
      cfg.vsync_pulse_width = 4;
      cfg.vsync_back_porch = 8;
      cfg.pclk_idle_high = 1;

      _bus_instance.config(cfg);
    }

    _panel_instance.setBus(&_bus_instance);

    {
      auto cfg = _touch_instance.config();
      cfg.x_min = 0;
      cfg.x_max = 800;
      cfg.y_min = 0;
      cfg.y_max = 480;
      cfg.pin_int = -1;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = I2C_NUM_0;
      cfg.pin_sda = GPIO_NUM_15;
      cfg.pin_scl = GPIO_NUM_16;
      cfg.pin_rst = -1;
      cfg.freq = 400000;
      cfg.i2c_addr = 0x5D;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }

    setPanel(&_panel_instance);
  }
};

LGFX lcd;
LGFX_Sprite radarCanvas(&lcd);

// CrowPanel Advance V1.5 board-controller command.
// The working Elecrow example uses I2C address 0x30.
void sendI2CCommand(uint8_t command) {
  Wire.beginTransmission(0x30);
  Wire.write(command);
  uint8_t error = Wire.endTransmission();

  if (error == 0) {
    DEBUG_PRINTF("I2C command %u sent successfully\n", command);
  } else {
    DEBUG_PRINTF("I2C command failed: %u\n", error);
  }
}

// ---------- LAYOUT ----------
const int MARGIN = 5;

const int RADAR_X = MARGIN;
const int RADAR_Y = MARGIN;
const int RADAR_W = 470;
const int RADAR_H = 470;

const int INFO_X = 485;
const int INFO_Y = MARGIN;
const int INFO_W = 310;
const int INFO_H = 470;

const int CX = 240;
const int CY = 240;

const float X_CORR = 0.93;
const int R = 212;

const int OUTER_GAP = 3;
const int OUTER_RING_THICKNESS = 3;

// ---------- SWEEP ----------
float sweepAngle = 0;
unsigned long lastSweepUpdate = 0;

const int TRAIL_WIDTH = 60;
const int SWEEP_SPEED = 60;

unsigned long lastClockUpdate = 0;

// ---------- COLORS ----------
uint16_t greenBright;
uint16_t green;
uint16_t red;
uint16_t greenDim;
uint16_t greenDark;
uint16_t panelBg;

// ---------- AIRCRAFT ----------
struct AircraftPoint {
  bool valid;
  float distanceKm;
  float bearingDeg;
  int altitudeFt;
  float speedKt;
  float trackDeg;
  char flight[12];
  char category[5];
  char typeCode[8];
};

const int MAX_PLANES = 20;

AircraftPoint planes[MAX_PLANES];
AircraftPoint pendingPlanes[MAX_PLANES];

int planeCount = 0;
int pendingPlaneCount = 0;

bool apiOk = false;
bool pendingApiOk = false;
bool fetchInProgress = false;
bool pendingDataReady = false;

// ---------- HELPERS ----------
int rxCorr(int radius) {
  return (int)(radius * X_CORR);
}

double deg2radD(double deg) {
  return deg * M_PI / 180.0;
}

double rad2degD(double rad) {
  return rad * 180.0 / M_PI;
}

float angleDiffBehindSweep(float sweep, float targetBearing) {
  float d = sweep - targetBearing;
  while (d < 0) d += 360;
  while (d >= 360) d -= 360;
  return d;
}

float distanceKm(double lat1, double lon1, double lat2, double lon2) {
  const double Rearth = 6371.0;

  double dLat = deg2radD(lat2 - lat1);
  double dLon = deg2radD(lon2 - lon1);

  double a =
    sin(dLat / 2) * sin(dLat / 2) +
    cos(deg2radD(lat1)) * cos(deg2radD(lat2)) *
    sin(dLon / 2) * sin(dLon / 2);

  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return Rearth * c;
}

float bearingDeg(double lat1, double lon1, double lat2, double lon2) {
  double y = sin(deg2radD(lon2 - lon1)) * cos(deg2radD(lat2));
  double x =
    cos(deg2radD(lat1)) * sin(deg2radD(lat2)) -
    sin(deg2radD(lat1)) * cos(deg2radD(lat2)) * cos(deg2radD(lon2 - lon1));

  double brng = rad2degD(atan2(y, x));
  if (brng < 0) brng += 360.0;
  return brng;
}

void canvasCorrectedCircle(int cx, int cy, int r, uint16_t col) {
  int lastX = cx + rxCorr(r);
  int lastY = cy;

  for (int a = 1; a <= 360; a++) {
    float rad = a * DEG_TO_RAD;
    int x = cx + cos(rad) * rxCorr(r);
    int y = cy + sin(rad) * r;

    radarCanvas.drawLine(lastX, lastY, x, y, col);

    lastX = x;
    lastY = y;
  }
}

void canvasCorrectedRadialLine(int angleDeg, int r1, int r2, uint16_t col) {
  float rad = (angleDeg - 90) * DEG_TO_RAD;

  int x1 = CX + cos(rad) * rxCorr(r1);
  int y1 = CY + sin(rad) * r1;
  int x2 = CX + cos(rad) * rxCorr(r2);
  int y2 = CY + sin(rad) * r2;

  radarCanvas.drawLine(x1, y1, x2, y2, col);
}

// ---------- API ----------
void fetchAircraftData() {

  unsigned long fetchStart = millis();
  DEBUG_PRINTLN("[FETCH] Starting aircraft download");
  
  DEBUG_PRINTF("[MEM] Free heap: %u\n", ESP.getFreeHeap());
  DEBUG_PRINTF("[MEM] Minimum free heap: %u\n", ESP.getMinFreeHeap());
  DEBUG_PRINTF("[MEM] Largest free block: %u\n", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  // Resolve DNS only when necessary or when the cached address expires.
  
  DEBUG_PRINT("[FETCH] adsbServerIPValid: ");
  DEBUG_PRINTLN(adsbServerIPValid);

  DEBUG_PRINT("[FETCH] Cached DNS Expired: ");
  DEBUG_PRINTLN(millis() - adsbDnsResolvedAt >= ADSB_DNS_REFRESH_MS);
  
  if (!adsbServerIPValid ||
    millis() - adsbDnsResolvedAt >= ADSB_DNS_REFRESH_MS) {

    if (!resolveADSBServer()) {

      DEBUG_PRINTLN("[FETCH] DNS unavailable - retaining previous data");

      pendingApiOk = false;
      pendingDataReady = true;

      return;
    }
  }   
  
  // not sure still need following code block given the above
  
  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_PRINTLN("[FETCH] WiFi disconnected - keeping previous aircraft data");
    pendingApiOk = false;
    pendingDataReady = true;

    return;
  }

  //String url = "https://api.airplanes.live/v2/point/";
  String url = "https://opendata.adsb.fi/api/v2/lat/";
  url += String(HOME_LAT, 4);
  url += "/lon/";
  url += String(HOME_LON, 4);
  url += "/dist/";
  url += String(API_RADIUS_NM);

  DEBUG_PRINTLN(("[FETCH] URL used:: " + url).c_str());

  WiFiClientSecure client;
  client.setInsecure();

  
  HTTPClient http;

  http.setTimeout(60000);
  http.setReuse(false);
  http.useHTTP10(true);

  http.addHeader("User-Agent", "ESP32-AirRadar/1.0");
  http.addHeader("Connection", "close");

  if (!http.begin(client, url)) {

    DEBUG_PRINTLN("[FETCH] HTTP initialisation failed");

    http.end();
    client.stop();

    pendingApiOk = false;
    pendingDataReady = true;

    return;
  }

  // Connect directly to the cached IP address.
  // Preserve the original hostname for TLS SNI.
  DEBUG_PRINTF("[HTTPS] Connecting to cached IP: %s\n",adsbServerIP.toString().c_str());

  if (!client.connect(
    adsbServerIP,
    443,
    "opendata.adsb.fi",
    nullptr,
    nullptr,
    nullptr)) {

    DEBUG_PRINTLN("[HTTPS] Cached IP connection failed");

    http.end();
    client.stop();

    // Force a fresh DNS lookup on the next fetch.
    
    adsbServerIPValid = false;
    DEBUG_PRINTLN("[DNS CACHE] Keeping cached IP after connection failure");

    pendingApiOk = false;
    pendingDataReady = true;

    return;
  }

  DEBUG_PRINTLN("[HTTPS] Connected using cached IP");

  int code = http.GET();

  const int contentLength = http.getSize();
  DEBUG_PRINTF("HTTP response code: %d, content length: %d bytes\n", code, contentLength);
  DEBUG_PRINTF("Free heap before JSON: %u bytes, free PSRAM: %u bytes\n", ESP.getFreeHeap(), ESP.getFreePsram());

  if (code != 200) {

    DEBUG_PRINTF("[FETCH] API HTTP error: %d\n", code);
    DEBUG_PRINTLN("[FETCH] Retaining previous aircraft data");

    http.end();
    client.stop();

    pendingApiOk = false;
    pendingDataReady = true;

    return;
  }

  // Keep only the fields used by the radar. The API response's aircraft
  // collection is under "aircraft"; "ac" is retained as a compatibility
  // fallback in case the provider changes format.
  StaticJsonDocument<1024> filter;
  const char* requiredFields[] = {
    "lat", "lon", "alt_baro", "alt_geom", "gs",
    "track", "flight", "category", "t"
  };
  for (const char* field : requiredFields) {
    filter["ac"][0][field] = true;
    filter["aircraft"][0][field] = true;
  }

  DynamicJsonDocument doc(65536);
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter) );
  
  // Always close the HTTP connection after parsing has consumed the body.
  http.end();
  client.stop();


  if (err) {

    DEBUG_PRINTF("[JSON] Parsing failed: %s\n", err.c_str());
    DEBUG_PRINTLN("[FETCH] Retaining previous aircraft data");
    DEBUG_PRINTF("Free heap after JSON error: %u bytes, free PSRAM: %u bytes\n", ESP.getFreeHeap(), ESP.getFreePsram());

    pendingApiOk = false;
    pendingDataReady = true;

    return;
  }

  DEBUG_PRINTF("Filtered JSON array sizes: ac=%u, aircraft=%u\n", (unsigned)doc["ac"].size(), (unsigned)doc["aircraft"].size());

  AircraftPoint tempPlanes[MAX_PLANES];
  int tempCount = 0;

  JsonArray arr = doc["ac"].as<JsonArray>();
  if (arr.isNull()) {
    arr = doc["aircraft"].as<JsonArray>();
  }

  // Keep the nearest MAX_PLANES aircraft, rather than whichever aircraft
  // happened to appear first in the API response.
  for (JsonObject ac : arr) {
    if (!ac["lat"].is<float>() || !ac["lon"].is<float>()) continue;

    double lat = ac["lat"].as<double>();
    double lon = ac["lon"].as<double>();

    float d = distanceKm(HOME_LAT, HOME_LON, lat, lon);
    if (d > RADAR_RANGE_KM) continue;

    AircraftPoint candidate = {};
    candidate.valid = true;
    candidate.distanceKm = d;
    candidate.bearingDeg = bearingDeg(HOME_LAT, HOME_LON, lat, lon);

    if (ac["alt_baro"].is<int>()) candidate.altitudeFt = ac["alt_baro"].as<int>();
    else if (ac["alt_geom"].is<int>()) candidate.altitudeFt = ac["alt_geom"].as<int>();
    else candidate.altitudeFt = -1;

    candidate.speedKt = ac["gs"] | 0.0f;
    candidate.trackDeg = ac["track"] | 0.0f;

    const char* fl = ac["flight"] | "";
    strncpy(candidate.flight, fl, sizeof(candidate.flight) - 1);
    candidate.flight[sizeof(candidate.flight) - 1] = '\0';

    const char* cat = ac["category"] | "";
    strncpy(candidate.category, cat, sizeof(candidate.category) - 1);
    candidate.category[sizeof(candidate.category) - 1] = '\0';

    const char* typ = ac["t"] | "";
    strncpy(candidate.typeCode, typ, sizeof(candidate.typeCode) - 1);
    candidate.typeCode[sizeof(candidate.typeCode) - 1] = '\0';

    // Find the insertion point in ascending distance order.
    int insertAt = 0;
    while (insertAt < tempCount &&
           tempPlanes[insertAt].distanceKm <= candidate.distanceKm) {
      insertAt++;
    }

    if (tempCount < MAX_PLANES) {
      for (int j = tempCount; j > insertAt; --j) {
        tempPlanes[j] = tempPlanes[j - 1];
      }
      tempPlanes[insertAt] = candidate;
      tempCount++;
    } else if (insertAt < MAX_PLANES) {
      for (int j = MAX_PLANES - 1; j > insertAt; --j) {
        tempPlanes[j] = tempPlanes[j - 1];
      }
      tempPlanes[insertAt] = candidate;
    }
  }

  DEBUG_PRINTF("JSON parsed. Nearest aircraft retained: %d\n", tempCount);
  DEBUG_PRINTF("Free heap after JSON: %u bytes, free PSRAM: %u bytes\n", ESP.getFreeHeap(), ESP.getFreePsram());

  noInterrupts();

  pendingPlaneCount = tempCount;

  for (int i = 0; i < tempCount; i++) {
    pendingPlanes[i] = tempPlanes[i];
  }

  pendingApiOk = true;
  pendingDataReady = true;

  interrupts();

  DEBUG_PRINTF("Background aircraft found: %d\n", tempCount);

  DEBUG_PRINTF("[FETCH] Completed in %lu ms\n", millis() - fetchStart);

  DEBUG_PRINTF("[MEM] Free heap after fetch: %u\n", ESP.getFreeHeap());
  DEBUG_PRINTF("[MEM] Minimum free heap: %u\n", ESP.getMinFreeHeap());
  DEBUG_PRINTF("[MEM] Largest free block: %u\n",heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

}

void aircraftFetchTask(void *parameter) {
  fetchAircraftData();
  fetchInProgress = false;
  vTaskDelete(NULL);
}

void startAircraftFetchBackground() {

  if (fetchInProgress) return;

  fetchInProgress = true;

  BaseType_t result = xTaskCreatePinnedToCore(
    aircraftFetchTask,
    "aircraftFetchTask",
    20000,
    NULL,
    1,
    NULL,
    0
  );

  if (result != pdPASS) {
    DEBUG_PRINTLN("[TASK] Failed to create aircraft fetch task");
    fetchInProgress = false;
  } else {
    DEBUG_PRINTLN("[TASK] Aircraft fetch task created");
  }




}
void applyPendingAircraftData() {

  if (!pendingDataReady) return;

  DEBUG_PRINTLN("[DATA] Applying pending aircraft update");

  noInterrupts();

  // Only replace aircraft data following a successful API request.
  if (pendingApiOk) {

    planeCount = pendingPlaneCount;

    for (int i = 0; i < planeCount; i++) {
      planes[i] = pendingPlanes[i];
    }

    DEBUG_PRINTF("[DATA] Updated aircraft: %d\n", planeCount);

  } else {

    DEBUG_PRINTLN("[DATA] API failed - retaining previous aircraft positions");

  }

  // Update API status independently of aircraft data.
  apiOk = pendingApiOk;

  pendingDataReady = false;

  interrupts();
}

// ---------- RADAR CANVAS DRAW ----------
void drawGridToCanvas() {
  radarCanvas.fillSprite(TFT_BLACK);

  for (int d = -240; d <= 240; d += 40) {
    int x = CX + rxCorr(d);
    if (x >= RADAR_X && x <= RADAR_X + RADAR_W) {
      radarCanvas.drawFastVLine(x, RADAR_Y, RADAR_H, greenDark);
    }
  }

  for (int d = -240; d <= 240; d += 40) {
    int y = CY + d;
    if (y >= RADAR_Y && y <= RADAR_Y + RADAR_H) {
      radarCanvas.drawFastHLine(RADAR_X, y, RADAR_W, greenDark);
    }
  }

  radarCanvas.drawRect(RADAR_X, RADAR_Y, RADAR_W, RADAR_H, green);
}

void drawSweepToCanvas() {
  for (float i = TRAIL_WIDTH; i >= 0; i -= 0.5) {
    float a = sweepAngle - i;
    if (a < 0) a += 360;

    float intensity = (float)(TRAIL_WIDTH - i) / TRAIL_WIDTH;

    uint8_t g = 6 + intensity * 120;
    uint16_t col = radarCanvas.color565(0, g, 0);

    float rad = (a - 90) * DEG_TO_RAD;

    int x = CX + cos(rad) * rxCorr(R);
    int y = CY + sin(rad) * R;

    radarCanvas.drawLine(CX, CY, x, y, col);
  }

  float rad = (sweepAngle - 90) * DEG_TO_RAD;
  int sx = CX + cos(rad) * rxCorr(R);
  int sy = CY + sin(rad) * R;

  radarCanvas.drawLine(CX, CY, sx, sy, greenBright);
  radarCanvas.drawLine(CX + 1, CY, sx + 1, sy, greenBright);
}

void drawRadarCirclesToCanvas() {
  canvasCorrectedCircle(CX, CY, 35, green);
  canvasCorrectedCircle(CX, CY, 70, green);
  canvasCorrectedCircle(CX, CY, 110, green);
  canvasCorrectedCircle(CX, CY, 150, green);
  canvasCorrectedCircle(CX, CY, 190, green);
  canvasCorrectedCircle(CX, CY, R, greenBright);

  for (int i = 0; i < OUTER_RING_THICKNESS; i++) {
    canvasCorrectedCircle(CX, CY, R + OUTER_GAP + 1 + i, greenBright);
  }
}

void drawCrossLinesToCanvas() {
  radarCanvas.drawFastHLine(CX - rxCorr(R), CY, rxCorr(R) * 2, greenBright);
  radarCanvas.drawFastVLine(CX, CY - R, R * 2, greenBright);

  for (int d = 20; d < R; d += 40) {
    radarCanvas.drawFastVLine(CX + rxCorr(d), CY - 7, 14, greenBright);
    radarCanvas.drawFastVLine(CX - rxCorr(d), CY - 7, 14, greenBright);

    radarCanvas.drawFastHLine(CX - 7, CY + d, 14, greenBright);
    radarCanvas.drawFastHLine(CX - 7, CY - d, 14, greenBright);
  }

  for (int a = 30; a < 360; a += 30) {
    if (a == 90 || a == 180 || a == 270) continue;
    canvasCorrectedRadialLine(a, 0, R, greenDim);
  }
}

void drawDistanceLabelsToCanvas() {
  radarCanvas.setTextDatum(middle_center);
  radarCanvas.setTextSize(1);
  radarCanvas.setTextColor(greenBright, TFT_BLACK);

  int radii[] = {35, 70, 110, 150, 190};
  int km[]    = {20, 40, 60, 80, 100};

  const int LABEL_OFFSET = 9;

  for (int i = 0; i < 5; i++) {
    radarCanvas.drawString(String(km[i]) + "km", CX + 18, CY - radii[i] - LABEL_OFFSET);
    radarCanvas.drawString(String(km[i]) + "km", CX + 18, CY + radii[i] + LABEL_OFFSET);
  }

  radarCanvas.drawString("0", CX + 15, CY + 12);
}

void drawAngleScaleToCanvas() {
  for (int a = 0; a < 360; a++) {
    int len = 4;
    if (a % 10 == 0) len = 13;
    else if (a % 5 == 0) len = 8;

    canvasCorrectedRadialLine(a, R - len, R, greenBright);
  }

  radarCanvas.setTextColor(greenBright, TFT_BLACK);
  radarCanvas.setTextSize(2);
  radarCanvas.setTextDatum(middle_center);

  for (int a = 0; a < 360; a += 30) {
    float rad = (a - 90) * DEG_TO_RAD;

    int tx = CX + cos(rad) * rxCorr(R - 28);
    int ty = CY + sin(rad) * (R - 28);

    radarCanvas.drawNumber(a, tx, ty);
  }
}

void drawAircraftTargetsToCanvas() {
  for (int i = 0; i < planeCount; i++) {
    if (!planes[i].valid) continue;

    float ageAngle = angleDiffBehindSweep(sweepAngle, planes[i].bearingDeg);

    if (ageAngle > 260) continue;

    float fade = 1.0;
    if (ageAngle > 200) {
      fade = 1.0 - ((ageAngle - 200.0) / 60.0);
      if (fade < 0.20) fade = 0.20;
    }

    float rr = (planes[i].distanceKm / RADAR_RANGE_KM) * R;
    float rad = (planes[i].bearingDeg - 90) * DEG_TO_RAD;

    int x = CX + cos(rad) * rxCorr(rr);
    int y = CY + sin(rad) * rr;

    uint8_t r = 0;
    uint8_t g = 255 * fade;
    uint8_t b = 35 * fade;

    if (planes[i].altitudeFt >= 30000) {
      r = 0; g = 180 * fade; b = 255 * fade;
    } else if (planes[i].altitudeFt >= 10000) {
      r = 0; g = 255 * fade; b = 35 * fade;
    } else if (planes[i].altitudeFt >= 0) {
      r = 255 * fade; g = 220 * fade; b = 0;
    } else {
      r = 255 * fade; g = 40 * fade; b = 40 * fade;
    }

    uint16_t col = radarCanvas.color565(r, g, b);

    int size = 4;
    if (planes[i].altitudeFt > 30000) size = 5;
    if (planes[i].distanceKm < 20) size = 6;

    radarCanvas.fillCircle(x, y, size, col);
    radarCanvas.drawCircle(x, y, size + 2, greenDim);

    if (planes[i].trackDeg >= 0) {
      float tr = (planes[i].trackDeg - 90) * DEG_TO_RAD;
      int x2 = x + cos(tr) * 12;
      int y2 = y + sin(tr) * 12;
      radarCanvas.drawLine(x, y, x2, y2, col);
    }

    // Callsign label only when sweep has just passed the aircraft
    if (ageAngle >= 0 && ageAngle < 25) {
      radarCanvas.setTextDatum(top_left);
      radarCanvas.setTextSize(1);
      radarCanvas.setTextColor(TFT_WHITE, TFT_BLACK);

      String label = String(planes[i].flight);
      label.trim();
      if (label.length() == 0) label = "UNKNOWN";

      radarCanvas.drawString(label, x + 9, y - 12);

 //     if (planes[i].altitudeFt >= 0) {
 //       radarCanvas.setTextColor(greenBright, TFT_BLACK);
 //       radarCanvas.drawString(String(planes[i].altitudeFt) + "ft", x + 9, y + 2);
 //     }
    }
  }

  // Top-left status - larger
  radarCanvas.setTextDatum(top_left);
  radarCanvas.setTextSize(2);

  if (apiOk) {
    radarCanvas.setTextColor(green, TFT_BLACK);
    radarCanvas.drawString("REAL ADS-B", 12, 10);

    radarCanvas.setTextColor(greenBright, TFT_BLACK); 
    radarCanvas.setTextSize(2);
    radarCanvas.drawString("AC: " + String(planeCount), 12, 32);
  } else {
    radarCanvas.setTextColor(radarCanvas.color565(255, 80, 80), TFT_BLACK);
    radarCanvas.drawString("NO API", 14, 14);
  }

  if (fetchInProgress) {
    radarCanvas.setTextSize(2);
    radarCanvas.setTextColor(greenDim, TFT_BLACK);
    radarCanvas.drawString("UPDATING", 10, 54);
  }

  // 3D-like compass bottom-left
  // 3D-like compass bottom-left - smaller
  int ccx = 50;
  int ccy = 427;
  uint16_t compassBright = greenBright;
  uint16_t compassDim = greenDim;
  uint16_t compassDark = greenDark;

  radarCanvas.setTextDatum(middle_center);

  // smaller outer ring
  radarCanvas.drawCircle(ccx, ccy, 17, compassDim);
  radarCanvas.drawCircle(ccx, ccy, 18, compassDark);

  // N triangle - bright
  radarCanvas.fillTriangle(ccx, ccy - 16, ccx - 5, ccy - 4, ccx + 5, ccy - 4, compassBright);
  radarCanvas.drawTriangle(ccx, ccy - 16, ccx - 5, ccy - 4, ccx + 5, ccy - 4, TFT_WHITE);

  // S triangle
  radarCanvas.fillTriangle(ccx, ccy + 16, ccx - 5, ccy + 4, ccx + 5, ccy + 4, compassDim);

  // W triangle
  radarCanvas.fillTriangle(ccx - 16, ccy, ccx - 4, ccy - 5, ccx - 4, ccy + 5, compassDim);

  // E triangle
  radarCanvas.fillTriangle(ccx + 16, ccy, ccx + 4, ccy - 5, ccx + 4, ccy + 5, compassDim);

  // center point
  radarCanvas.fillCircle(ccx, ccy, 2, compassBright);
  radarCanvas.drawCircle(ccx, ccy, 4, compassDim);

  // smaller letters
  radarCanvas.setTextSize(1);
  radarCanvas.setTextColor(compassBright, TFT_BLACK);
  radarCanvas.drawString("N", ccx, ccy - 29);

  radarCanvas.setTextColor(compassDim, TFT_BLACK);
  radarCanvas.drawString("S", ccx, ccy + 29);
  radarCanvas.drawString("W", ccx - 29, ccy - 3);
  radarCanvas.drawString("E", ccx + 29, ccy - 3);
}

void drawRangeBarsToCanvas() {
  // 5 bars: 0-20, 20-40, 40-60, 60-80, 80-100 km
  const int bins = 5;
  int counts[bins] = {0};

  for (int i = 0; i < planeCount; i++) {
    if (!planes[i].valid) continue;

    int bin = (int)(planes[i].distanceKm / 20.0);
    if (bin < 0) bin = 0;
    if (bin >= bins) bin = bins - 1;

    counts[bin]++;
  }

  const int startX = 395;
  const int baseY  = 62;
  const int barW   = 10;
  const int barGap = 14;

  const int tickH = 3;
  const int tickGap = 2;
  const int maxTicks = 8;

  radarCanvas.setTextDatum(top_left);
  radarCanvas.setTextSize(1);
  radarCanvas.setTextColor(green, TFT_BLACK);
  radarCanvas.drawString("RANGE AC", startX - 0, 12);

  for (int b = 0; b < bins; b++) {
    int x = startX + b * barGap;
    int c = counts[b];
    if (c > maxTicks) c = maxTicks;

    for (int t = 0; t < c; t++) {
      int y = baseY - t * (tickH + tickGap);
      radarCanvas.fillRect(x, y, barW, tickH, greenBright);
    }

    // faint zero baseline
    radarCanvas.drawFastHLine(x, baseY + 7, barW, greenDark);
  }

  // labels only 20 / 60 / 100
  radarCanvas.setTextColor(green, TFT_BLACK);
  radarCanvas.drawString("20",  startX - 1,             baseY + 14);
  radarCanvas.drawString("60",  startX + 2 * barGap - 1, baseY + 14);
  radarCanvas.drawString("100", startX + 4 * barGap - 4, baseY + 14);
}

void drawSweepHeadingIndicatorToCanvas() {
  int hx = 430;
  int hy = 420;

  int angleInt = (int)sweepAngle;
  if (angleInt < 0) angleInt += 360;
  if (angleInt >= 360) angleInt -= 360;

  radarCanvas.setTextDatum(middle_center);

  // small dial
  radarCanvas.drawCircle(hx, hy, 28, greenDark);
  radarCanvas.drawCircle(hx, hy, 29, greenDim);

  // rotating pointer triangle
  float a = (sweepAngle - 90) * DEG_TO_RAD;

  int tipX = hx + cos(a) * 23;
  int tipY = hy + sin(a) * 23;

  float leftA  = a + 2.45;
  float rightA = a - 2.45;

  int leftX = hx + cos(leftA) * 10;
  int leftY = hy + sin(leftA) * 10;
  int rightX = hx + cos(rightA) * 10;
  int rightY = hy + sin(rightA) * 10;

  radarCanvas.fillTriangle(tipX, tipY, leftX, leftY, rightX, rightY, greenBright);
  radarCanvas.drawLine(hx, hy, tipX, tipY, TFT_WHITE);

  radarCanvas.fillCircle(hx, hy, 3, greenBright);

  // degree number
  radarCanvas.setTextSize(2);
  radarCanvas.setTextColor(greenBright, TFT_BLACK);

  char buf[8];
  sprintf(buf, "%03d", angleInt);
  radarCanvas.drawString(buf, hx, hy + 42);

  radarCanvas.setTextSize(1);
  radarCanvas.setTextColor(green, TFT_BLACK);
  radarCanvas.drawString("SWEEP", hx, hy - 38);
}

void drawRadarFrameToCanvas() {
  drawGridToCanvas();
  drawSweepToCanvas();
  drawRadarCirclesToCanvas();
  drawCrossLinesToCanvas();
  drawDistanceLabelsToCanvas();
  drawAngleScaleToCanvas();
  drawAircraftTargetsToCanvas();
  drawRangeBarsToCanvas();
  drawSweepHeadingIndicatorToCanvas();
}

void drawTimePanel() {
  int boxX = INFO_X + 18;
  int boxY = INFO_Y + 392;
  int boxW = INFO_W - 36;
  int boxH = 68;

  lcd.fillRect(boxX, boxY, boxW, boxH, TFT_BLACK);

  lcd.drawFastHLine(INFO_X + 10, boxY - 17, INFO_W - 20, greenDark);

  lcd.setTextDatum(top_left);
  lcd.setTextSize(2);
  lcd.setTextColor(greenBright, TFT_BLACK);
  lcd.drawString("TIME:", boxX+110, boxY-7);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeStr[12];
    sprintf(timeStr, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    lcd.setTextSize(5);
    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.drawString(timeStr, boxX+20, boxY + 22);
  } else {
    lcd.setTextSize(4);
    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.drawString("--:--:--", boxX+20, boxY + 22);
  }
}

void drawLocationPanel() {
  int boxX = INFO_X + 18;
  int boxY = INFO_Y + 305;   // како што кажа
  int boxW = INFO_W - 36;
  int boxH = 70;

  lcd.fillRect(boxX, boxY, boxW, boxH, TFT_BLACK);

  // 🔹 хоризонтална линија над TIME (ова е болдираната линија што ти фали)
  lcd.drawFastHLine(INFO_X + 10, boxY - 10, INFO_W - 20, greenDark);

  // OHRID - центрирано
  lcd.setTextDatum(top_center);
  lcd.setTextSize(2);
  lcd.setTextColor(greenBright, TFT_BLACK);
  lcd.drawString("OHRID", boxX + boxW / 2, boxY);

  // Lat / Long во ЕДЕН ред
  lcd.setTextDatum(top_center);
  lcd.setTextSize(2);
  lcd.setTextColor(TFT_YELLOW, TFT_BLACK);

  String coord = String(HOME_LAT, 4) + " / " + String(HOME_LON, 4);
  lcd.drawString(coord, boxX + boxW / 2, boxY + 32);
}

void drawDetectedAircraftPanel() {
  int topY = INFO_Y + 10;       // почнува најгоре
  int bottomY = INFO_Y + 295;   // до пред OHRID
  int boxX = INFO_X + 10;
  int boxW = INFO_W - 20;
  int boxH = bottomY - topY;

  lcd.fillRect(boxX, topY, boxW, boxH, TFT_BLACK);

  lcd.setTextDatum(top_center);
  lcd.setTextSize(2);
  lcd.setTextColor(greenBright, TFT_BLACK);
  lcd.drawString("DETECTED AIRCRAFTS", INFO_X + INFO_W / 2, topY + 2);

  const int MAX_ROWS = 10;
  int idx[MAX_ROWS];
  for (int i = 0; i < MAX_ROWS; i++) idx[i] = -1;

  // најди 8 најблиски авиони
  for (int i = 0; i < planeCount; i++) {
    if (!planes[i].valid) continue;

    for (int j = 0; j < MAX_ROWS; j++) {
      if (idx[j] == -1 || planes[i].distanceKm < planes[idx[j]].distanceKm) {
        for (int k = MAX_ROWS - 1; k > j; k--) idx[k] = idx[k - 1];
        idx[j] = i;
        break;
      }
    }
  }

  lcd.setTextDatum(top_left);
  lcd.setTextSize(1);

  int y = topY + 34;

  for (int row = 0; row < MAX_ROWS; row++) {
    if (idx[row] == -1) break;

    AircraftPoint &p = planes[idx[row]];

    String call = String(p.flight);
    call.trim();
    if (call.length() == 0) call = "UNKNOWN";

    uint16_t dotCol = greenBright;
    if (p.altitudeFt >= 30000) dotCol = lcd.color565(0, 180, 255);
    else if (p.altitudeFt >= 10000) dotCol = lcd.color565(0, 255, 35);
    else if (p.altitudeFt >= 0) dotCol = lcd.color565(255, 220, 0);
    else dotCol = lcd.color565(255, 40, 40);

    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.drawString(call.substring(0, 7), boxX + 2, y);

    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.drawString(String((int)p.distanceKm) + "km", boxX + 76, y);

    if (p.altitudeFt >= 0) lcd.drawString(String(p.altitudeFt) + "ft", boxX + 118, y);
    else lcd.drawString("---ft", boxX + 118, y);

    lcd.drawString(String((int)p.speedKt) + "kt", boxX + 188, y);

    lcd.setTextColor(green, TFT_BLACK);
    
   String typ = String(p.typeCode);
typ.trim();
if (typ.length() == 0) typ = String(p.category);

lcd.drawString(typ.substring(0, 5), boxX + 235, y);

    lcd.fillCircle(boxX + 272, y + 5, 4, dotCol);

    lcd.drawFastHLine(boxX, y + 17, boxW, greenDark);

    y += 26;
  }

  if (planeCount == 0) {
    lcd.setTextDatum(top_center);
    lcd.setTextSize(1);
    lcd.setTextColor(greenDim, TFT_BLACK);
    lcd.drawString("NO AIRCRAFT IN RANGE", INFO_X + INFO_W / 2, topY + 52);
  }
}
// ---------- RIGHT PANEL ----------

void drawRightInfoPanel() {
  lcd.fillRect(INFO_X, INFO_Y, INFO_W, INFO_H, TFT_BLACK);
  lcd.drawRect(INFO_X, INFO_Y, INFO_W, INFO_H, green);

  lcd.setTextDatum(top_left);

//  lcd.setTextSize(2);
 // lcd.setTextColor(greenBright, TFT_BLACK);
 // lcd.drawString("AIR TRAFFIC RADAR", INFO_X + 18, INFO_Y + 18);

//  lcd.drawFastHLine(INFO_X + 10, INFO_Y + 55, INFO_W - 20, greenDark);

  drawDetectedAircraftPanel();
  drawLocationPanel();
  drawTimePanel();
}

bool resolveADSBServer() {

    DEBUG_PRINTLN("[DNS] Resolving ADS-B server...");

    IPAddress newIP;

    if (WiFi.hostByName("opendata.adsb.fi", newIP) == 1) {   //this the function that's failing intermittently

        adsbServerIP = newIP;
        adsbServerIPValid = true;
        adsbDnsResolvedAt = millis();

        DEBUG_PRINTF("[DNS] ADS-B server IP: %s\n", adsbServerIP.toString().c_str());

        return true;
    }
    // attempt to reestablish WiFi and try one more time
    DEBUG_PRINTLN("[DNS] Initial attempt failed to reconencting WiFi and will try again");
    // WiFi.disconnect();
    // delay(1000);
    WiFi.reconnect();
    delay(2000);
    if (WiFi.hostByName("opendata.adsb.fi", newIP) == 1) {   //this the function that's failing intermittently

        adsbServerIP = newIP;
        adsbServerIPValid = true;
        adsbDnsResolvedAt = millis();

        DEBUG_PRINTF("[DNS] ADS-B server IP: %s\n", adsbServerIP.toString().c_str());

        return true;
    }



    DEBUG_PRINTLN("[DNS] Resolution failed");
    DEBUG_PRINTLN("[DNS] Retaining previously cached IP");

    // Do not invalidate an existing cached IP.
    //return adsbServerIPValid;
    return false;
}


// ---------- SETUP ----------

void setup() {
  Serial.begin(115200);
  delay(1000);
  DEBUG_PRINTLN("Starting CrowPanel Advance V1.5 radar...");

  // Keep the same startup sequence that successfully displayed the RGB colour test.
  pinMode(19, OUTPUT);
  Wire.begin(15, 16);
  delay(50);

  DEBUG_PRINTLN("Checking I2C devices...");

  while (true) {
    Wire.beginTransmission(0x30);
    bool controllerOK = (Wire.endTransmission() == 0);

    Wire.beginTransmission(0x5D);
    bool touchOK = (Wire.endTransmission() == 0);

    if (controllerOK && touchOK) {
      DEBUG_PRINTLN("I2C devices detected");
      break;
    }

    DEBUG_PRINTLN("Waiting for I2C devices...");
    sendI2CCommand(250);

    pinMode(1, OUTPUT);
    digitalWrite(1, LOW);
    delay(120);
    pinMode(1, INPUT);

    delay(100);
  }

  // Turn on the display backlight using the board controller.
  sendI2CCommand(0);
  delay(100);

  lcd.init();
  DEBUG_PRINTLN("Display initialised");

  greenBright = lcd.color565(0, 255, 35);
  green       = lcd.color565(0, 180, 25);
  red         = lcd.color565(255, 0, 0);
  greenDim    = lcd.color565(0, 90, 18);
  greenDark   = lcd.color565(0, 45, 10);
  panelBg     = lcd.color565(0, 20, 0);

  radarCanvas.setPsram(true);
  radarCanvas.setColorDepth(16);

  if (!radarCanvas.createSprite(480, 480)) {
    DEBUG_PRINTLN("ERROR: Radar sprite allocation failed. Check PSRAM and board settings.");
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(TFT_RED, TFT_BLACK);
    lcd.setTextSize(2);
    lcd.setTextDatum(middle_center);
    lcd.drawString("RADAR MEMORY ERROR", 400, 240);
    while (true) {
      delay(1000);
    }
  }

  lcd.fillScreen(TFT_BLACK);

  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setTextSize(2);
  lcd.setTextDatum(middle_center);
  lcd.drawString("Connecting WiFi...", 240, 240);

  DEBUG_PRINTLN("Trying primary Wifi connection...");

  WiFi.begin(ssid, password);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    DEBUG_PRINTLN("Waiting for primary WiFi...");
    delay(300);
  }

  // DEBUG_PRINTLN("Trying secondary Wifi connection...");

  // WiFi.begin(ssid2, password2);

  // wifiStart = millis();
  // while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
  //   DEBUG_PRINTLN("Waiting for secondary WiFi...");
  //   delay(300);
  // }

  lcd.fillScreen(TFT_BLACK);

  if (WiFi.status() == WL_CONNECTED) {

      DEBUG_PRINTLN("WiFi connected");

      configTzTime("GMT0BST,M3.5.0/1,M10.5.0/2",
             "pool.ntp.org",
             "time.nist.gov");           


      DEBUG_PRINTLN("Starting initial aircraft data fetch...");

      // Run HTTPS/API operations in a separate FreeRTOS task.
      startAircraftFetchBackground();

  } else {

      DEBUG_PRINTLN("WiFi failed");
      apiOk = false;
  }



  drawRadarFrameToCanvas();
  radarCanvas.pushSprite(0, 0);

  drawRightInfoPanel();
}







// ---------- LOOP ----------
void loop() {
  if (millis() - lastSweepUpdate > SWEEP_SPEED) {
    lastSweepUpdate = millis();

    drawRadarFrameToCanvas();  
    radarCanvas.pushSprite(0, 0);

    sweepAngle += 2;

    if (sweepAngle >= 360) {
      sweepAngle -= 360;

      applyPendingAircraftData();
      drawDetectedAircraftPanel();
      startAircraftFetchBackground();
    }
  }

  if (millis() - lastClockUpdate > 1000) {
    lastClockUpdate = millis();
    drawTimePanel();
  }
}