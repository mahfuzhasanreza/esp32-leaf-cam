#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_camera.h>

// WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Pi 5 server endpoint
const char* pi5_url = "http://<PI5_IP>:5000/infer"; // Replace <PI5_IP> with your Pi 5 IP

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("WiFi connected");

  // Camera init (use your board's config)
  camera_config_t config;
  // ... fill config for your ESP32-CAM ...
  esp_camera_init(&config);
}

void loop() {
  // Capture image
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    delay(2000);
    return;
  }

  // Send image to Pi 5
  HTTPClient http;
  http.begin(pi5_url);
  http.addHeader("Content-Type", "image/jpeg");
  int httpResponseCode = http.POST(fb->buf, fb->len);
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Pi response: " + response);
    // Optionally forward to ESP32 hub
  } else {
    Serial.println("Error sending image: " + String(httpResponseCode));
  }
  http.end();

  esp_camera_fb_return(fb);
  delay(10000); // Wait before next capture
}
