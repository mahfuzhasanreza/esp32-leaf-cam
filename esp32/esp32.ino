// ========= ESP32-CAM (sender) =========
#include "esp_camera.h"
#include <Arduino.h>

// --- Select your camera pins (AI Thinker) ---
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define FLASH_GPIO         4

// UART to ESP32 hub:
#define CAM_UART_BAUD   2000000   // fast & stable on short wires
#define CAM_UART_RX        3       // U0RXD
#define CAM_UART_TX        1       // U0TXD

// simple CRC16 (Modbus-ish)
static uint16_t crc16(const uint8_t* data, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      if (crc & 1) crc = (crc >> 1) ^ 0xA001;
      else crc >>= 1;
    }
  }
  return crc;
}

void setup() {
  pinMode(FLASH_GPIO, OUTPUT);
  digitalWrite(FLASH_GPIO, LOW);

  Serial.begin(115200);
  delay(200);

  // UART0 is already on pins 1/3; just set speed
  Serial.flush();
  Serial.end();
  delay(50);
  Serial.begin(CAM_UART_BAUD);
  delay(100);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size   = FRAMESIZE_VGA;    // 640x480
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;               // ~good JPG size
  config.fb_count     = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    // blink flash on error forever
    while (true) {
      digitalWrite(FLASH_GPIO, HIGH); delay(100);
      digitalWrite(FLASH_GPIO, LOW);  delay(100);
    }
  }
}

void loop() {
  // Wait for a single-byte command 'C'
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'C') {
      // small pre-flash
      digitalWrite(FLASH_GPIO, HIGH);
      delay(80);

      camera_fb_t* fb = esp_camera_fb_get();
      digitalWrite(FLASH_GPIO, LOW);

      if (!fb || fb->len < 8) {
        const char errSig[4] = {'P','V','I','E'};
        Serial.write((const uint8_t*)errSig, 4);
        uint32_t zero = 0;
        Serial.write((uint8_t*)&zero, 4);
        uint16_t zcrc = 0;
        Serial.write((uint8_t*)&zcrc, 2);
        if (fb) esp_camera_fb_return(fb);
        return;
      }

      // header
      const char magic[4] = {'P','V','I','C'};
      Serial.write((const uint8_t*)magic, 4);
      // big-endian length
      uint32_t L = fb->len;
      uint8_t lenBE[4] = { (uint8_t)(L>>24), (uint8_t)(L>>16), (uint8_t)(L>>8), (uint8_t)L };
      Serial.write(lenBE, 4);

      // crc16
      uint16_t crc = crc16(fb->buf, fb->len);
      uint8_t crcBE[2] = { (uint8_t)(crc>>8), (uint8_t)crc };
      Serial.write(crcBE, 2);

      // body
      Serial.write(fb->buf, fb->len);

      esp_camera_fb_return(fb);
    } else {
      // drain unexpected
      while (Serial.available()) Serial.read();
    }
  }
}
