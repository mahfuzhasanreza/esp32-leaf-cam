#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <vector>
#include <cstring>

// ====== User wiring/config ======
// Button on ESP32 GPIO14 to GND (uses INPUT_PULLUP)
static const int PIN_BUTTON = 14; // per request

// UART2 pins between ESP32 hub and ESP32-CAM (UART0 on ESP32-CAM = GPIO1 TX, GPIO3 RX)
// Connect: HUB TX (GPIO17) -> CAM RX0 (GPIO3), HUB RX (GPIO16) <- CAM TX0 (GPIO1), common GND
static const int CAM_RX_PIN = 16; // HUB RX pin
static const int CAM_TX_PIN = 17; // HUB TX pin
static const uint32_t CAM_BAUD = 921600; // must match camera (more robust)

// I2C OLED (SSD1306 128x64) on default ESP32 I2C pins SDA=21, SCL=22
static const int OLED_WIDTH = 128;
static const int OLED_HEIGHT = 64;
static const int OLED_RESET = -1; // no reset line
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);

// WiFi credentials (fill in your network)
const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

// ====== Protocol with camera ======
// Camera sends: 'P''V''I''C' + 4-byte BE length + 2-byte BE CRC16 + JPEG bytes
// Command: single byte 'C' from hub to camera

HardwareSerial CamSerial(2); // UART2
WebServer server(80);

// Store last image in RAM
static std::vector<uint8_t> lastImage;
static uint32_t lastImageCrc = 0;

// simple CRC16 (Modbus-ish), must match camera
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

static void oledMsg(const String& l1, const String& l2 = "", const String& l3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(l1);
  if (l2.length()) display.println(l2);
  if (l3.length()) display.println(l3);
  display.display();
}

static bool readExact(uint8_t* buf, size_t n, uint32_t timeoutMs) {
  uint32_t start = millis();
  size_t got = 0;
  while (got < n) {
    if (CamSerial.available()) {
      got += CamSerial.readBytes(buf + got, n - got);
      start = millis(); // activity resets timeout
    } else if (millis() - start > timeoutMs) {
      return false;
    }
    delay(0);
  }
  return true;
}

// Find header magic 'PVIC' within a timeout, ignoring any noise
static bool readHeader(uint32_t headerTimeoutMs, uint32_t& outLen, uint16_t& outCrc, String& outErr) {
  const uint8_t MAGIC[4] = {'P','V','I','C'};
  const uint8_t ERRMG[4] = {'P','V','I','E'};
  uint8_t window[4] = {0};
  uint32_t start = millis();
  size_t filled = 0;
  while (millis() - start <= headerTimeoutMs) {
    if (CamSerial.available()) {
      uint8_t b = (uint8_t)CamSerial.read();
      if (filled < 4) {
        window[filled++] = b;
      } else {
        window[0] = window[1];
        window[1] = window[2];
        window[2] = window[3];
        window[3] = b;
      }
      if (filled == 4 && std::memcmp(window, MAGIC, 4) == 0) {
        // Read len + crc
        uint8_t rest[6];
        if (!readExact(rest, sizeof(rest), 3000)) { outErr = "timeout len+crc"; return false; }
        uint32_t len = (uint32_t)rest[0]<<24 | (uint32_t)rest[1]<<16 | (uint32_t)rest[2]<<8 | (uint32_t)rest[3];
        uint16_t crc = (uint16_t)rest[4]<<8 | (uint16_t)rest[5];
        if (len == 0 || len > 400000) { outErr = "bad length"; return false; }
        outLen = len; outCrc = crc; return true;
      }
      if (filled == 4 && std::memcmp(window, ERRMG, 4) == 0) {
        // Camera reported error
        uint8_t rest[6];
        if (!readExact(rest, sizeof(rest), 2000)) { outErr = "timeout len+crc (err)"; return false; }
        outErr = "camera error";
        return false;
      }
    } else {
      delay(1);
    }
  }
  outErr = "timeout header";
  return false;
}

static bool captureFromCam(uint32_t& outLen, uint16_t& outCrc, String& outErr) {
  // Send trigger
  while (CamSerial.available()) CamSerial.read();
  CamSerial.write('C');
  CamSerial.flush();

  // Wait and read header with sliding window
  uint32_t len = 0; uint16_t crc = 0;
  if (!readHeader(8000, len, crc, outErr)) return false;

  // Read body
  lastImage.clear();
  lastImage.resize(len);
  if (!readExact(lastImage.data(), len, 12000)) { outErr = "timeout body"; lastImage.clear(); return false; }

  // Validate CRC
  uint16_t calc = crc16(lastImage.data(), lastImage.size());
  if (calc != crc) { outErr = "crc mismatch"; lastImage.clear(); return false; }

  outLen = len;
  outCrc = crc;
  lastImageCrc = crc;
  return true;
}

// Web handlers
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>ESP32 Camera Dashboard</title>
    <style>
      body { font-family: system-ui, sans-serif; margin: 20px; }
      button { padding: 10px 16px; font-size: 16px; }
      img { max-width: 100%; height: auto; display: block; margin-top: 16px; border: 1px solid #ddd; }
    </style>
  </head>
  <body>
    <h1>ESP32 Camera Dashboard</h1>
    <button onclick="capture()">Capture</button>
    <img id="img" src="/image.jpg?ts=0" alt="No image yet" />
    <script>
      async function capture(){
        try {
          await fetch('/capture');
          const ts = Date.now();
          document.getElementById('img').src = '/image.jpg?ts='+ts;
        } catch(e){ alert('Capture failed'); }
      }
    </script>
  </body>
</html>
)HTML";

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleCapture() {
  oledMsg("Capturing...", "Please wait");
  uint32_t len=0; uint16_t crc=0;
  String err;
  bool ok = captureFromCam(len, crc, err);
  if (ok) {
    oledMsg("Capture OK", String(len)+" bytes", "CRC:"+String(crc,16));
    server.send(200, "text/plain", "OK");
  } else {
    oledMsg("Capture FAILED", err);
    server.send(500, "text/plain", String("FAIL: ")+err);
  }
}

void handleImage() {
  if (lastImage.empty()) {
    server.send(404, "text/plain", "No image");
    return;
  }
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.setContentLength(lastImage.size());
  server.send(200, "image/jpeg", "");
  WiFiClient client = server.client();
  client.write(lastImage.data(), lastImage.size());
}

// Debounce
bool lastBtn = true; // pull-up idle HIGH
uint32_t lastChange = 0;

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(PIN_BUTTON, INPUT_PULLUP);

  // OLED init
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    // If OLED not found, continue without it
  } else {
    display.clearDisplay();
    display.display();
    oledMsg("Booting...");
  }

  // UART to camera
  CamSerial.begin(CAM_BAUD, SERIAL_8N1, CAM_RX_PIN, CAM_TX_PIN);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  oledMsg("WiFi connecting", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    oledMsg("WiFi OK", ip);
  } else {
    oledMsg("WiFi FAIL", "Starting AP: cam-hub");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("cam-hub", "12345678");
  }

  // Web server routes
  server.on("/", handleRoot);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/image.jpg", HTTP_GET, handleImage);
  server.begin();
}

void loop() {
  server.handleClient();

  // Button handling (active LOW)
  bool b = digitalRead(PIN_BUTTON);
  if (b != lastBtn) {
    lastChange = millis();
    lastBtn = b;
  }
  if (!b && (millis() - lastChange) > 40) { // pressed
    // One-shot capture on press
    uint32_t len=0; uint16_t crc=0;
    oledMsg("Button pressed", "Capturing...");
    String err;
    bool ok = captureFromCam(len, crc, err);
    if (ok) {
      oledMsg("Capture OK", String(len)+" bytes");
    } else {
      oledMsg("Capture FAILED", err);
    }
    // wait for release to avoid repeats
    while (digitalRead(PIN_BUTTON) == LOW) { delay(10); server.handleClient(); }
  }
}
