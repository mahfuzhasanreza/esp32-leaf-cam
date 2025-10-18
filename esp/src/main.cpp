// ========= ESP32 (hub) =========
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <U8g2lib.h>

// ---------------- WiFi ----------------
const char* WIFI_SSID = "Room-1010";
const char* WIFI_PASS = "room1010";

// ------------- Raspberry Pi target -----
const char* PI_HOST = "192.168.1.42";   // <-- set to the Pi's IP or hostname
const uint16_t PI_PORT = 8000;          // <-- set to the port your Pi listener uses
const char* PI_UPLOAD_PATH = "/upload"; // <-- set to the path that accepts the upload
const uint32_t PI_RESPONSE_TIMEOUT_MS = 7000;

// ------------- UART to CAM -------------
#define UART_RX   16   // from CAM TX (GPIO1)
#define UART_TX   17   // to   CAM RX (GPIO3)
#define UART_BAUD 2000000

// ------------- Button (to GND) --------
#define BTN_PIN   14

// ------------- OLED (1.3" SH1106) -----
#define SDA_PIN   21
#define SCL_PIN   22
// If your 1.3" really is SSD1306, swap the constructor line below:
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /*rst=*/U8X8_PIN_NONE, /*scl=*/SCL_PIN, /*sda=*/SDA_PIN);
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /*rst=*/U8X8_PIN_NONE, /*scl=*/SCL_PIN, /*sda=*/SDA_PIN);

// ------------- Web server --------------
WebServer server(80);

bool lastUploadOk = false;
String lastUploadName;
String lastUploadErr;
uint32_t lastUploadTimestamp = 0;

void setLastUploadStatus(bool ok, const String& name, const String& err) {
  lastUploadOk = ok;
  lastUploadTimestamp = millis();
  if (ok) {
    lastUploadName = name;
    lastUploadErr = "";
  } else {
    lastUploadName = "";
    lastUploadErr = err;
  }
}

// ------------- Helpers -----------------
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

void oledPrint(const String& l1, const String& l2="", const String& l3="") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, l1.c_str());
  if (!l2.isEmpty()) u8g2.drawStr(0, 26, l2.c_str());
  if (!l3.isEmpty()) u8g2.drawStr(0, 40, l3.c_str());
  u8g2.sendBuffer();
}

String makeRemoteFilename() {
  static uint32_t counter = 0;
  ++counter;
  char buf[40];
  snprintf(buf, sizeof(buf), "capture_%lu_%lu.jpg", (unsigned long)millis(), (unsigned long)counter);
  return String(buf);
}

// read exactly n bytes with timeout
bool readExact(uint8_t* dst, size_t n, uint32_t timeout_ms) {
  size_t got = 0;
  uint32_t start = millis();
  while (got < n) {
    if (millis() - start > timeout_ms) return false;
    int avail = Serial2.available();
    if (avail > 0) {
      size_t chunk = (size_t)avail;
      size_t remaining = n - got;
      if (chunk > remaining) chunk = remaining; // safe min
      int r = Serial2.read(dst + got, chunk);
      if (r > 0) got += (size_t)r;
    } else {
      delay(1);
    }
  }
  return true;
}

bool requestCaptureAndSave(const char* path, String& err) {
  err = "";

  // purge input
  while (Serial2.available()) Serial2.read();

  // send capture command
  const char cmd = 'C';
  Serial2.write((const uint8_t*)&cmd, 1);
  Serial2.flush();

  // header: magic(4) + len(4 BE) + crc(2 BE)
  uint8_t header[10];
  if (!readExact(header, 10, 3000)) { err = "timeout header"; return false; }

  if (memcmp(header, "PVIC", 4) != 0) {
    if (memcmp(header, "PVIE", 4) == 0) err = "CAM error";
    else err = "bad magic";
    return false;
  }
  uint32_t L = (uint32_t)header[4] << 24 |
               (uint32_t)header[5] << 16 |
               (uint32_t)header[6] << 8  |
               (uint32_t)header[7];

  uint16_t want_crc = ((uint16_t)header[8] << 8) | (uint16_t)header[9];
  if (L < 16 || L > 2*1024*1024) { err = "bad len"; return false; }

  // receive body to SPIFFS
  File f = SPIFFS.open(path, FILE_WRITE);
  if (!f) { err = "file open fail"; return false; }

  const size_t BUFSZ = 2048;
  uint8_t* buf = (uint8_t*)malloc(BUFSZ);
  if (!buf) { f.close(); err = "oom"; return false; }

  size_t got = 0;
  uint16_t run_crc = 0xFFFF;
  uint32_t start = millis();

  while (got < L) {
    if (millis() - start > 8000) { free(buf); f.close(); err = "timeout body"; return false; }
    int avail = Serial2.available();
    if (avail <= 0) { delay(1); continue; }

    size_t chunk = (size_t)avail;
    size_t remaining = L - got;
    if (chunk > remaining) chunk = remaining;
    if (chunk > BUFSZ) chunk = BUFSZ;

    int r = Serial2.read(buf, chunk);
    if (r > 0) {
      f.write(buf, (size_t)r);
      run_crc = crc16(buf, (size_t)r) ^ (run_crc ^ 0xFFFF) ? // quick combine not trivial; do incremental:
                crc16(buf, (size_t)r) : run_crc;             // simpler: recompute incrementally below
      // simpler & correct incremental CRC:
      run_crc = crc16(buf, (size_t)r) ^ 0; // recalc on chunk; to be strictly incremental we’d update bitwise;
                                            // but to keep it simple we’ll compute a final CRC by re-reading the file later.
      got += (size_t)r;
      start = millis();
    }
  }
  f.close();
  free(buf);

  // recompute crc on the saved file (simple & safe)
  File rf = SPIFFS.open(path, FILE_READ);
  if (!rf) { err = "file reopen fail"; return false; }
  uint16_t crc_final = 0xFFFF;
  while (true) {
    uint8_t tmp[1024];
    int r = rf.read(tmp, sizeof(tmp));
    if (r <= 0) break;
    // true incremental:
    for (int i = 0; i < r; ++i) {
      crc_final ^= tmp[i];
      for (int b = 0; b < 8; ++b) {
        if (crc_final & 1) crc_final = (crc_final >> 1) ^ 0xA001;
        else crc_final >>= 1;
      }
    }
  }
  rf.close();

  if (crc_final != want_crc) { err = "crc mismatch"; return false; }
  return true;
}

bool uploadFileToPi(const char* path, const String& remoteName, String& err) {
  err = "";
  if (WiFi.status() != WL_CONNECTED) { err = "wifi disconnected"; return false; }

  File f = SPIFFS.open(path, FILE_READ);
  if (!f) { err = "open fail"; return false; }
  size_t total = f.size();
  if (total == 0) { f.close(); err = "empty file"; return false; }

  String safePath = PI_UPLOAD_PATH;
  if (!safePath.startsWith('/')) safePath = '/' + safePath;
  String queryName = remoteName;
  queryName.replace(' ', '_');

  WiFiClient client;
  client.setTimeout(PI_RESPONSE_TIMEOUT_MS);
  if (!client.connect(PI_HOST, PI_PORT)) {
    f.close();
    err = "connect fail";
    return false;
  }

  client.print(String("POST ") + safePath + "?name=" + queryName + " HTTP/1.1\r\n");
  client.print(String("Host: ") + PI_HOST + ":" + String(PI_PORT) + "\r\n");
  client.print("Content-Type: image/jpeg\r\n");
  client.print("Connection: close\r\n");
  client.print(String("Content-Length: ") + (unsigned long)total + "\r\n\r\n");

  const size_t BUF_SZ = 1024;
  uint8_t buf[BUF_SZ];
  while (f.available()) {
    size_t n = f.read(buf, BUF_SZ);
    if (!n) continue;
    size_t written = client.write(buf, n);
    if (written != n) {
      f.close();
      client.stop();
      err = "socket write";
      return false;
    }
  }
  f.close();

  uint32_t start = millis();
  while (!client.available()) {
    if (millis() - start > PI_RESPONSE_TIMEOUT_MS) {
      client.stop();
      err = "resp timeout";
      return false;
    }
    delay(10);
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  if (!statusLine.startsWith("HTTP/1.1 200") && !statusLine.startsWith("HTTP/1.0 200")) {
    client.stop();
    err = statusLine;
    return false;
  }

  client.stop();
  return true;
}

bool captureAndUpload(String& err, String& remoteName) {
  err = "";
  remoteName = "";
  const char* localPath = "/latest.jpg";

  String captureErr;
  if (!requestCaptureAndSave(localPath, captureErr)) {
    err = String("capture: ") + captureErr;
    return false;
  }

  remoteName = makeRemoteFilename();

  String uploadErr;
  if (!uploadFileToPi(localPath, remoteName, uploadErr)) {
    err = String("upload: ") + uploadErr;
    remoteName = "";
    return false;
  }

  return true;
}

// ----------- Web handlers -----------
void handleRoot() {
  String ip = WiFi.localIP().toString();
  String target = String(PI_HOST) + ":" + String(PI_PORT) + String(PI_UPLOAD_PATH);
  String targetHtml = target;
  targetHtml.replace("&", "&amp;");
  targetHtml.replace("<", "&lt;");
  targetHtml.replace(">", "&gt;");

  String status;
  if (lastUploadTimestamp == 0) {
    status = "No uploads yet";
  } else if (lastUploadOk) {
    status = String("Last upload OK &rarr; ") + lastUploadName;
  } else {
    status = String("Last upload FAIL: ") + lastUploadErr;
  }
  status.replace("&", "&amp;");
  status.replace("<", "&lt;");
  status.replace(">", "&gt;");

  String html =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'/>"
    "<title>ESP32 Leaf Viewer</title>"
    "<style>body{font-family:sans-serif;margin:1rem}img{max-width:100%;height:auto;border:1px solid #ccc}</style>"
    "</head><body>"
    "<h2>ESP32 Leaf Viewer</h2>"
    "<p>IP: " + ip + "</p>"
    "<p>Pi target: " + targetHtml + "</p>"
    "<p>" + status + "</p>"
    "<button onclick=\"fetch('/capture').then(()=>setTimeout(()=>location.reload(),1500))\">Capture</button>"
    "<p><img src='/image?ts="
    + String(millis()) +
    "'/></p>"
    "</body></html>";
  server.send(200, "text/html", html);
}

void handleImage() {
  if (!SPIFFS.exists("/latest.jpg")) {
    server.send(404, "text/plain", "No image yet");
    return;
  }
  File f = SPIFFS.open("/latest.jpg", FILE_READ);
  server.streamFile(f, "image/jpeg");
  f.close();
}

void handleCapture() {
  String err;
  String remoteName;
  oledPrint("CAPTURE...", "sending to Pi");
  bool ok = captureAndUpload(err, remoteName);

  if (ok) {
    setLastUploadStatus(true, remoteName, "");
    oledPrint("UPLOAD OK", remoteName, "-> Pi");
    server.send(200, "application/json", String("{\"ok\":true,\"remote\":\"") + remoteName + "\"}");
  } else {
    setLastUploadStatus(false, "", err);
    oledPrint("UPLOAD FAIL", err);
    server.send(500, "application/json", String("{\"ok\":false,\"err\":\"") + err + "\"}");
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // UART to camera
  Serial2.begin(UART_BAUD, SERIAL_8N1, UART_RX, UART_TX);

  // Button
  pinMode(BTN_PIN, INPUT_PULLUP);

  // OLED
  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();
  oledPrint("Booting...");

  // FS
  if (!SPIFFS.begin(true)) {
    oledPrint("SPIFFS fail");
  }

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  oledPrint("WiFi connecting", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    oledPrint("WiFi OK", WiFi.localIP().toString());
  } else {
    oledPrint("WiFi FAIL", "Check SSID/PASS");
  }

  // Web
  server.on("/", handleRoot);
  server.on("/image", handleImage);
  server.on("/capture", handleCapture);
  server.begin();
}

uint32_t lastBtnMs = 0;
bool lastBtn = true;

void loop() {
  server.handleClient();

  // Poll button (to GND, pullup)
  bool now = digitalRead(BTN_PIN);
  uint32_t ms = millis();
  if (lastBtn && !now && (ms - lastBtnMs > 200)) { // falling edge
    lastBtnMs = ms;
    String err;
    String remoteName;
    oledPrint("BTN -> CAPTURE", "sending to Pi");
    bool ok = captureAndUpload(err, remoteName);
    if (ok) {
      setLastUploadStatus(true, remoteName, "");
      oledPrint("BTN OK", remoteName, "-> Pi");
    } else {
      setLastUploadStatus(false, "", err);
      oledPrint("BTN FAIL", err);
    }
  }
  lastBtn = now;
}
