#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
// Forward declaration for OLED message function
void oledMsg(const String& l1, const String& l2 = "", const String& l3 = "");
// Pi 5 server endpoints (set your Pi 5 IP)
#define PI5_UPLOAD_URL "http://10.141.5.128:8000/upload"
// Optional polling endpoint if you implement it on Pi:
#define PI5_RESULT_URL "http://10.141.5.128:8000/result"
// LED and buzzer pins
#define GREEN_LED_PIN 27
#define RED_LED_PIN 26
#define BUZZER_PIN 25

static bool gProcessingActive = false;
static unsigned long gLastRedBlinkMs = 0;
static bool gRedBlinkState = false;
static bool gGreenPulseActive = false;
static unsigned long gGreenPulseEndMs = 0;
static bool gBuzzerActive = false;
static unsigned long gBuzzerEndMs = 0;
static bool gWaitingForResult = false;
static bool gResultDisplayed = false;
static String gPendingTimestamp;
static String gPendingLeaf;
static String gPendingDisease;
static String gPendingSolution;
static String gDisplayedTimestamp;

void updateIndicators();
void clearProcessingState();
void startGreenPulse(unsigned long durationMs = 1500);
void startBuzzerPulse(unsigned long durationMs = 5000);

void showResultOnOLED(const String& leaf, const String& disease, const String& solution) {
  clearProcessingState();
  String safeLeaf = leaf.length() ? leaf : "Unknown Leaf";
  String safeDisease = disease.length() ? disease : "Unknown";
  String safeSolution = solution.length() ? solution : "No advice";
  oledMsg("Leaf: " + safeLeaf, "Disease: " + safeDisease, "Solution: " + safeSolution);
  gResultDisplayed = true;
  gWaitingForResult = false;
  gPendingTimestamp = "";
  gPendingLeaf = safeLeaf;
  gPendingDisease = safeDisease;
  gPendingSolution = safeSolution;
  startGreenPulse(1500);
  startBuzzerPulse(400);
}

void setProcessingState() {
  gProcessingActive = true;
  gLastRedBlinkMs = millis();
  gRedBlinkState = true;
  digitalWrite(RED_LED_PIN, HIGH);
  gGreenPulseActive = false;
  gGreenPulseEndMs = 0;
  gWaitingForResult = false;
  gResultDisplayed = false;
  gPendingTimestamp = "";
  gPendingLeaf = "";
  gPendingDisease = "";
  gPendingSolution = "";
  digitalWrite(GREEN_LED_PIN, LOW);
  gBuzzerActive = false;
  gBuzzerEndMs = 0;
  digitalWrite(BUZZER_PIN, LOW);
}

void clearProcessingState() {
  gProcessingActive = false;
  gRedBlinkState = false;
  digitalWrite(RED_LED_PIN, LOW);
  gWaitingForResult = false;
}

void startGreenPulse(unsigned long durationMs) {
  gGreenPulseActive = true;
  gGreenPulseEndMs = millis() + durationMs;
  digitalWrite(GREEN_LED_PIN, HIGH);
}

void startBuzzerPulse(unsigned long durationMs) {
  gBuzzerActive = true;
  gBuzzerEndMs = millis() + durationMs;
  digitalWrite(BUZZER_PIN, HIGH);
}

void updateIndicators() {
  unsigned long now = millis();

  if (gProcessingActive) {
    if (now - gLastRedBlinkMs >= 250) {
      gRedBlinkState = !gRedBlinkState;
      digitalWrite(RED_LED_PIN, gRedBlinkState ? HIGH : LOW);
      gLastRedBlinkMs = now;
    }
  } else if (gRedBlinkState) {
    gRedBlinkState = false;
    digitalWrite(RED_LED_PIN, LOW);
  }

  if (gGreenPulseActive && (long)(now - gGreenPulseEndMs) >= 0) {
    gGreenPulseActive = false;
    digitalWrite(GREEN_LED_PIN, LOW);
  }

  if (gBuzzerActive && (long)(now - gBuzzerEndMs) >= 0) {
    gBuzzerActive = false;
    digitalWrite(BUZZER_PIN, LOW);
  }
}
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SH110X.h>
#include <vector>
#include <cstring>

// ====== User wiring/config ======
// Button on ESP32 GPIO14 to GND (uses INPUT_PULLUP)
static const int PIN_BUTTON = 14; // per request

// UART2 pins between ESP32 hub and ESP32-CAM (UART0 on ESP32-CAM = GPIO1 TX, GPIO3 RX)
// Connect: HUB TX (GPIO17) -> CAM RX0 (GPIO3), HUB RX (GPIO16) <- CAM TX0 (GPIO1), common GND
static const int CAM_RX_PIN = 16; // HUB RX pin
static const int CAM_TX_PIN = 17; // HUB TX pin
static const uint32_t CAM_BAUD = 921600; // must match ESP32-CAM sketch baud rate

// I2C OLED on default ESP32 I2C pins SDA=21, SCL=22
// Compile-time config (overridable via platformio.ini build_flags):
//  - USE_SH1106: 1 for SH1106 controller (typical 1.3"), 0 for SSD1306
//  - OLED_ADDR: I2C address (0x3C default, sometimes 0x3D)
//  - OLED_WIDTH/OLED_HEIGHT: resolution (defaults 128x64)
#ifndef USE_SH1106
#define USE_SH1106 1
#endif
#ifndef OLED_ADDR
#define OLED_ADDR 0x3C
#endif
#ifndef OLED_WIDTH
#define OLED_WIDTH 128
#endif
#ifndef OLED_HEIGHT
#define OLED_HEIGHT 64
#endif
static const int OLED_RESET = -1; // set to RST pin if available
#if USE_SH1106
Adafruit_SH1106G display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
#else
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
#endif

// WiFi credentials (fill in your network)
const char* WIFI_SSID = "PK";
const char* WIFI_PASS = "provat07";

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

// Upload current JPEG in memory to Pi 5
static bool uploadToPi(const uint8_t* jpg,
                       size_t len,
                       String& outErr,
                       String* outLeaf = nullptr,
                       String* outDisease = nullptr,
                       String* outSolution = nullptr,
                       String* outTimestamp = nullptr,
                       bool* outHasResult = nullptr) {
  if (WiFi.status() != WL_CONNECTED) {
    outErr = "WiFi not connected";
    Serial.println(F("[uploadToPi] WiFi not connected"));
    return false;
  }
  HTTPClient http;
  http.setTimeout(10000);
  WiFiClient wifiClient;
  if (!http.begin(wifiClient, PI5_UPLOAD_URL)) {
    outErr = "HTTP begin failed";
    Serial.println(F("[uploadToPi] http.begin failed"));
    return false;
  }
  http.addHeader("Content-Type", "image/jpeg");
  int code = http.POST(const_cast<uint8_t*>(jpg), len);
  if (code <= 0) {
    outErr = String("HTTP error ") + http.errorToString(code);
    Serial.printf("[uploadToPi] POST failed: %s\n", outErr.c_str());
    http.end();
    return false;
  }
  if (code != 200) {
    outErr = String("Upload failed ") + code;
    Serial.printf("[uploadToPi] Non-OK response code %d\n", code);
    http.end();
    return false;
  }
  Serial.printf("[uploadToPi] Uploaded %u bytes -> %d\n", (unsigned)len, code);
  String body = http.getString();
  http.end();

  // Try parse JSON response for optional fields
  DynamicJsonDocument doc(2048);
  DeserializationError jerr = deserializeJson(doc, body);
  bool hasResult = false;
  if (!jerr) {
    String leaf = doc["leaf_name"] | doc["species"] | "";
    String diseaseVal = doc["disease"] | doc["condition"] | "";
    String solutionVal = doc["solution"] | doc["recommendation"] | "";
    if (outLeaf) {
      *outLeaf = leaf;
    }
    if (outDisease) {
      *outDisease = diseaseVal;
    }
    if (outSolution) {
      *outSolution = solutionVal;
    }
    if (leaf.length() || diseaseVal.length() || solutionVal.length()) {
      hasResult = true;
    }
    if (outTimestamp) {
      const char* tsVal = doc["timestamp"] | "";
      *outTimestamp = tsVal;
    }
  }
  if (outHasResult) {
    *outHasResult = hasResult;
  }
  return true;
}

void oledMsg(const String& l1, const String& l2, const String& l3) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  int maxChars = (OLED_WIDTH / 6); // 6 pixels per char at text size 1
  int maxLines = (OLED_HEIGHT / 8); // 8 pixels per line at text size 1
  // Collect all input strings into one vector
  std::vector<String> inputLines;
  if (l1.length()) inputLines.push_back(l1);
  if (l2.length()) inputLines.push_back(l2);
  if (l3.length()) inputLines.push_back(l3);
  // Split each input line into chunks that fit the display
  std::vector<String> displayLines;
  for (auto &line : inputLines) {
    int pos = 0;
    while (pos < line.length()) {
      displayLines.push_back(line.substring(pos, pos + maxChars));
      pos += maxChars;
    }
  }
  // Print only as many lines as fit the screen
  for (int i = 0; i < maxLines && i < displayLines.size(); ++i) {
    display.setCursor(0, i * 8);
    display.print(displayLines[i]);
  }
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
    updateIndicators();
    delay(0);
  }
  return true;
}

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
    updateIndicators();
  }
  outErr = "timeout header";
  return false;
}

static bool captureFromCam(uint32_t& outLen, uint16_t& outCrc, String& outErr) {
  // Send trigger
  while (CamSerial.available()) { CamSerial.read(); updateIndicators(); }
  Serial.println(F("[captureFromCam] Triggering camera"));
  CamSerial.write('C');
  CamSerial.flush();

  // Wait and read header with sliding window
  uint32_t len = 0; uint16_t crc = 0;
  if (!readHeader(8000, len, crc, outErr)) {
    Serial.printf("[captureFromCam] Header failure: %s\n", outErr.c_str());
    return false;
  }

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

// Web UI
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
  setProcessingState();
  gResultDisplayed = false;
  gWaitingForResult = false;
  oledMsg("Capturing...", "Please wait");
  uint32_t len=0; uint16_t crc=0;
  String err;
  bool ok = captureFromCam(len, crc, err);
  if (ok) {
    // Try uploading to Pi 5
    String leaf, disease, solution, timestamp, uerr;
    bool hasResult = false;
    bool up = uploadToPi(
      lastImage.data(),
      lastImage.size(),
      uerr,
      &leaf,
      &disease,
      &solution,
      &timestamp,
      &hasResult
    );
    if (up) {
      if (hasResult) {
        String displayLeaf = leaf.length() ? leaf : "Unknown Leaf";
        String displayDisease = disease.length() ? disease : "Unknown";
        String displaySolution = solution.length() ? solution : "No advice";
        String displayTimestamp = timestamp.length() ? timestamp : String(millis());
        showResultOnOLED(displayLeaf, displayDisease, displaySolution);
        gDisplayedTimestamp = displayTimestamp;

        DynamicJsonDocument resp(512);
        resp["ok"] = true;
        resp["uploaded"] = true;
        resp["bytes"] = len;
        resp["leaf_name"] = displayLeaf;
        resp["disease"] = displayDisease;
        resp["solution"] = displaySolution;
        resp["timestamp"] = displayTimestamp;
        String body;
        serializeJson(resp, body);
        server.send(200, "application/json", body);
      } else {
        gPendingLeaf = leaf.length() ? leaf : "OK";
        gPendingDisease = disease;
        gPendingSolution = solution;
        gPendingTimestamp = timestamp;
        if (!gPendingTimestamp.length()) {
          gPendingTimestamp = String(millis());
        }
        gWaitingForResult = true;
        gResultDisplayed = false;
        oledMsg("Processing...", "Waiting for Pi");

        DynamicJsonDocument resp(256);
        resp["ok"] = true;
        resp["uploaded"] = true;
        resp["bytes"] = len;
        resp["waiting"] = true;
        String body;
        serializeJson(resp, body);
        server.send(200, "application/json", body);
      }
    } else {
      clearProcessingState();
      digitalWrite(RED_LED_PIN, HIGH);
      digitalWrite(GREEN_LED_PIN, LOW);
      oledMsg("Upload failed", uerr, String(len) + " bytes saved");
      gWaitingForResult = false;
      gResultDisplayed = false;
      server.send(200, "application/json", String("{\"ok\":true,\"uploaded\":false,\"err\":\"") + uerr + "\"}");
    }
  } else {
    clearProcessingState();
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, LOW);
    oledMsg("Capture FAILED", err);
    gWaitingForResult = false;
    gResultDisplayed = false;
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

// Capture and immediately return JPEG
void handleCaptureJpg() {
  setProcessingState();
  oledMsg("Capturing...", "Please wait");
  uint32_t len=0; uint16_t crc=0; String err;
  if (!captureFromCam(len, crc, err)) {
    clearProcessingState();
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, LOW);
    oledMsg("Capture FAILED", err);
    server.send(500, "text/plain", String("FAIL: ")+err);
    return;
  }
  clearProcessingState();
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
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  clearProcessingState();
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  Serial.begin(115200);
  delay(100);

  pinMode(PIN_BUTTON, INPUT_PULLUP);

  // OLED init with auto-detect (0x3C / 0x3D)
  Wire.begin(21, 22);
  Wire.setClock(400000);
  uint8_t detectedAddr = 0;
  for (uint8_t a : { (uint8_t)0x3C, (uint8_t)0x3D }) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { detectedAddr = a; break; }
  }
  if (!detectedAddr) detectedAddr = (uint8_t)OLED_ADDR;
#if USE_SH1106
  if (!display.begin(detectedAddr, true)) {
    // continue without OLED
  } else {
    display.clearDisplay();
    display.display();
    oledMsg("Booting...");
  }
#else
  if (!display.begin(SSD1306_SWITCHCAPVCC, detectedAddr)) {
    // continue without OLED
  } else {
    display.clearDisplay();
    display.display();
    oledMsg("Booting...");
  }
#endif

  // UART to camera
  CamSerial.begin(CAM_BAUD, SERIAL_8N1, CAM_RX_PIN, CAM_TX_PIN);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  oledMsg("WiFi connecting", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    updateIndicators();
  }
  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    oledMsg("WiFi OK", ip);
  } else {
    oledMsg("WiFi FAIL", "AP: cam-hub");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("cam-hub", "12345678");
  }

  // Web server routes
  server.on("/", handleRoot);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/image.jpg", HTTP_GET, handleImage);
  server.on("/capture.jpg", HTTP_GET, handleCaptureJpg);
  server.begin();
}

void loop() {
  updateIndicators();
  server.handleClient();
  static unsigned long lastPoll = 0;
  if (WiFi.status() == WL_CONNECTED && millis() - lastPoll > 5000) {
    lastPoll = millis();
    HTTPClient http;
    http.begin(PI5_RESULT_URL);
    int httpResponseCode = http.GET();
    if (httpResponseCode > 0) {
      String response = http.getString();
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, response);
      if (!error && !doc["error"]) {
        String leaf = doc["leaf_name"] | doc["species"] | "Unknown";
        String disease = doc["disease"] | doc["condition"] | "Unknown";
        String solution = doc["solution"] | doc["recommendation"] | "No advice";
        String timestamp = doc["timestamp"] | "";

        bool shouldDisplay = false;
        if (gWaitingForResult) {
          if (gPendingTimestamp.length()) {
            if (timestamp == gPendingTimestamp) {
              if (!gResultDisplayed || timestamp != gDisplayedTimestamp) {
                shouldDisplay = true;
              }
            }
          } else if (!gResultDisplayed) {
            shouldDisplay = true;
          }
        } else if (timestamp.length() && timestamp != gDisplayedTimestamp) {
          shouldDisplay = true;
        }

        if (shouldDisplay) {
          String displayLeaf = leaf.length() ? leaf : gPendingLeaf;
          String displayDisease = disease.length() ? disease : gPendingDisease;
          String displaySolution = solution.length() ? solution : gPendingSolution;
          showResultOnOLED(displayLeaf, displayDisease, displaySolution);
          if (timestamp.length()) {
            gDisplayedTimestamp = timestamp;
          } else {
            gDisplayedTimestamp = String(millis());
          }
        }
      } else if (!gWaitingForResult) {
        clearProcessingState();
      }
    } else if (!gWaitingForResult) {
      clearProcessingState();
    }
    http.end();
  }

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
    setProcessingState();
    gResultDisplayed = false;
    gWaitingForResult = false;
    String err;
    bool ok = captureFromCam(len, crc, err);
    if (ok) {
      // Upload to Pi 5 after capture
      String leaf, disease, solution, timestamp, uerr;
      bool hasResult = false;
      if (uploadToPi(
            lastImage.data(),
            lastImage.size(),
            uerr,
            &leaf,
            &disease,
            &solution,
            &timestamp,
            &hasResult)) {
        if (hasResult) {
          String displayLeaf = leaf.length() ? leaf : "Unknown Leaf";
          String displayDisease = disease.length() ? disease : "Unknown";
          String displaySolution = solution.length() ? solution : "No advice";
          String displayTimestamp = timestamp.length() ? timestamp : String(millis());
          showResultOnOLED(displayLeaf, displayDisease, displaySolution);
          gDisplayedTimestamp = displayTimestamp;
        } else {
          gPendingLeaf = leaf.length() ? leaf : "OK";
          gPendingDisease = disease;
          gPendingSolution = solution;
          gPendingTimestamp = timestamp;
          if (!gPendingTimestamp.length()) {
            gPendingTimestamp = String(millis());
          }
          gWaitingForResult = true;
          gResultDisplayed = false;
          oledMsg("Processing...", "Waiting for Pi");
        }
      } else {
        clearProcessingState();
        digitalWrite(RED_LED_PIN, HIGH);
        digitalWrite(GREEN_LED_PIN, LOW);
        oledMsg("Upload failed", uerr, String(len) + " bytes saved");
        gWaitingForResult = false;
        gResultDisplayed = false;
      }
    } else {
      clearProcessingState();
      digitalWrite(RED_LED_PIN, HIGH);
      digitalWrite(GREEN_LED_PIN, LOW);
      oledMsg("Capture FAILED", err);
      gWaitingForResult = false;
      gResultDisplayed = false;
    }
    // wait for release to avoid repeats
    while (digitalRead(PIN_BUTTON) == LOW) { delay(10); server.handleClient(); updateIndicators(); }
  }
}
