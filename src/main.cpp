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

#define LGFX_USE_V1

#include <Arduino.h>
#include <Wire.h>
#include "time.h"
#include <LittleFS.h>
#include "esp_heap_caps.h"

#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

#include <driver/i2c.h>
#include <ArduinoJson.h>

//define font
#define latin Latin_Hiragana_24
// #define small NotoSansBold15
#define small &fonts::Font2
#define medium &fonts::Font3
#define large &fonts::Font4
#define extraLarge &fonts::Font6
#define sevenSegment &fonts::Font7
#define largeNueric &fonts::Font8

//define color
#define c1 0xE73C //white
#define c2 0x18C3  //gray
#define c3 0x9986 //red
#define c4 0x2CAB  //green
#define c5 0xBDEF //gold
#define c6 0xFFFAFA //snow

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
LGFX_Sprite canvas(&lcd);

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


// ---------- COLORS ----------
uint16_t greenBright;
uint16_t green;
uint16_t red;
uint16_t greenDim;
uint16_t greenDark;
uint16_t panelBg;


// ---------- SETUP ----------


// ---------- LOOP ------------

#define LGFX_AUTODETECT
#include <LovyanGFX.hpp>

LGFX_Sprite dataTicker(&lcd);

// Ticker configuration
#define DATATICKER_X       0
#define DATATICKER_Y       380
#define DATATICKER_WIDTH   800
#define DATATICKER_HEIGHT  120

#define SCROLL_DELAY   1

// Ticker variable

float dataScrollX = DATATICKER_WIDTH;
const float SCROLL_SPEED = 80.0f;  // Pixels per second 
unsigned long lastScroll = 0;



const char* dataMessage =
    "Alt. 15000' Speed 315kt Bearing 219deg Distance 12nm ";

//int16_t dataScrollX;
int16_t dataMessageWidth;

//uint32_t lastScroll = 0;

void setup()
{
  Serial.begin(115200);
  delay(1000);
  DEBUG_PRINTLN("Starting CrowPanel Advance V1.5 ...");

  if (!LittleFS.begin())
  {
    Serial.println("LittleFS mount FAILED!");
  }
  else
  {
    Serial.println("LittleFS mounted successfully!");

    File root = LittleFS.open("/");

    File file = root.openNextFile();

    while (file)
    {
      Serial.print("Found file: ");
      Serial.println(file.name());

      file = root.openNextFile();
    }
}





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


  lcd.init();
  lcd.setRotation(0);

  // Draw static display
  lcd.fillScreen(TFT_BLACK);

  // Use a built-in font initially
  
  //lcd.setFont(&fonts::FreeSansBold24pt7b);   TEMP
  
  Serial.println("Opening Calibri font...");

  File fontFile = LittleFS.open("/Calibri-Bold-120.vlw", "r");

  if (!fontFile)
  {
      Serial.println("Cannot open font!");
      return;
  }

  size_t fontSize = fontFile.size();

  Serial.printf("Font size: %u bytes\n", fontSize);

  
  lcd.fillScreen(TFT_BLACK);


  // Row 1

  if (lcd.loadFont(LittleFS, "/Calibri-Bold-100.vlw"))
  {
    Serial.println("Font loaded");

    // lcd.setTextDatum(middle_center);
    lcd.setTextDatum(top_left);
    lcd.setTextColor(TFT_RED);
    lcd.drawString("British Airways", 0, 50);

    Serial.println("Drawing completed");

    lcd.unloadFont();
  }
  else
  {
      Serial.println("Font loading failed");
  }

  // Row 2

  if (lcd.loadFont(LittleFS, "/Calibri-Bold-60.vlw"))
  {
    Serial.println("Font loaded");

    // lcd.setTextDatum(middle_center);
    lcd.setTextDatum(top_left);
    lcd.setTextColor(TFT_BLUE);
    lcd.drawString("BAW859  BOEING 737-800", 0, 200);


    Serial.println("Drawing completed");

    lcd.unloadFont();
  }
  else
  {
    Serial.println("Font loading failed");
  }

 // Row 2a

  if (lcd.loadFont(LittleFS, "/Calibri-Bold-50.vlw"))
  {
    Serial.println("Font loaded");

    // lcd.setTextDatum(middle_center);
    lcd.setTextDatum(top_left);
    lcd.setTextColor(TFT_CYAN);
    lcd.drawString("Prague (LKPR) / London (EGLL) ", 0, 300);

    Serial.println("Drawing completed");

    lcd.unloadFont();
  }
  else
  {
    Serial.println("Font loading failed");
  }

 
 // Row 3 - prepare the sprite used to manage the scrolling display
 
  // Create ticker sprites
  dataTicker.setColorDepth(16);
  dataTicker.setColor(TFT_GREEN);
  dataTicker.loadFont(LittleFS, "/Calibri-Bold-50.vlw");
  
  dataTicker.setTextWrap(false);

  dataTicker.createSprite(DATATICKER_WIDTH, DATATICKER_HEIGHT);

  //dataTicker.setTextSize(1);
  
  // Calculate text width
  dataMessageWidth = dataTicker.textWidth(dataMessage);

  // Start text outside the right edge
  dataScrollX = DATATICKER_WIDTH;

  dataTicker.fillSprite(TFT_BLACK);


}
/*
void loop()
{
  if (millis() - lastScroll >= SCROLL_DELAY)
  {
    lastScroll = millis();

    // Clear only ticker area
    dataTicker.fillSprite(TFT_BLACK);

    // Draw text at scrolling position
    dataTicker.setTextColor(TFT_GREEN);
    dataTicker.setCursor(dataScrollX, 0);
    dataTicker.print(dataMessage);

    // Move text left
    dataScrollX -= SCROLL_SPEED;

    // Restart after entire message leaves the screen
    if (dataScrollX < -dataMessageWidth)
    {
      dataScrollX = DATATICKER_WIDTH;
    }

    // Update only ticker rectangle
    dataTicker.pushSprite(DATATICKER_X, DATATICKER_Y);
}

  // delay(1000);
}*/

void loop()
{
    unsigned long now = millis();

    if (lastScroll == 0)
    {
        lastScroll = now;
        return;
    }

    float elapsed = (now - lastScroll) / 1000.0f;

    // Update scrolling position
    dataScrollX -= SCROLL_SPEED * elapsed;

    lastScroll = now;

    // Clear ticker area
    dataTicker.fillSprite(TFT_BLACK);

    // Draw scrolling text
    dataTicker.setTextColor(TFT_GREEN);
    dataTicker.setCursor((int)dataScrollX, 0);
    dataTicker.print(dataMessage);

    // Restart after entire message leaves screen
    if (dataScrollX < -dataMessageWidth)
    {
        dataScrollX = DATATICKER_WIDTH;
    }

    // Update ticker display
    dataTicker.pushSprite(DATATICKER_X, DATATICKER_Y);
}