#include <WiFi.h>
#include <Network.h>
#include <math.h>

constexpr char kSsid[] = "PullUp-S3";
constexpr char kPassword[] = "pullup-demo";
constexpr uint16_t kPort = 8765;
constexpr uint32_t kSampleIntervalMs = 40;  // 25 Hz
constexpr bool kUseSimulatedRoll = true;

NetworkServer server(kPort);
NetworkClient client;
uint32_t lastSampleMs = 0;
bool apReady = false;

void setupImu() {
  // Initialize your IMU and its orientation filter here if using real data.
}

bool readImuRollDegrees(float &degrees) {
  // Set degrees to the filtered X-axis roll in degrees and return true for a
  // fresh sample. Return false while the sensor is offline or has no sample.
  // Example with a sensor library: degrees = imu.getRoll(); return true;
  return false;
}

bool readRollDegrees(float &degrees, uint32_t now) {
  if (kUseSimulatedRoll) {
    const float cycle = (now % 6000) / 6000.0f;
    degrees = -60.0f * cosf(2.0f * PI * cycle);
    return true;
  }
  return readImuRollDegrees(degrees);
}

void setup() {
  Serial.begin(115200);
  if (!kUseSimulatedRoll) {
    setupImu();
    Serial.println("IMU mode. Implement the IMU hooks for your sensor.");
  }
  WiFi.mode(WIFI_AP);
  const IPAddress address(192, 168, 4, 1);
  if (!WiFi.softAPConfig(address, address, IPAddress(255, 255, 255, 0)) ||
      !WiFi.softAP(kSsid, kPassword)) {
    Serial.println("Could not start the Wi-Fi access point.");
    return;
  }
  server.begin();
  apReady = true;
  Serial.printf("Join %s, then connect to %s:%u (%s roll)\n",
                kSsid, WiFi.softAPIP().toString().c_str(), unsigned(kPort),
                kUseSimulatedRoll ? "simulated" : "IMU");
}

void loop() {
  if (!apReady) {
    delay(250);
    return;
  }
  if (!client.connected()) {
    client.stop();
    client = server.accept();
    if (client) {
      client.setNoDelay(true);
      Serial.println("Viewer connected.");
    }
  }

  const uint32_t now = millis();
  if (now - lastSampleMs >= kSampleIntervalMs) {
    lastSampleMs = now;
    float degrees;
    // Poll the IMU even without a connected viewer so the filter can settle.
    if (readRollDegrees(degrees, now) && isfinite(degrees) && client.connected()) {
      degrees = fmodf(degrees, 360.0f);
      if (degrees > 180.0f) degrees -= 360.0f;
      if (degrees < -180.0f) degrees += 360.0f;
      char line[32];
      const int length = snprintf(line, sizeof(line), "ROLL,%+.2f\n", degrees);
      if (length > 0 && length < int(sizeof(line)) &&
          client.write(reinterpret_cast<const uint8_t *>(line), size_t(length)) != size_t(length)) {
        client.stop();
      }
    }
  }
  delay(1);
}
