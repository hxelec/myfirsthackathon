#include <Arduino.h>
#include <WiFi.h>
#include <Network.h>
#include <SD.h>
#include <SPI.h>
#include <math.h>

constexpr char kSsid[] = "LiftWatch-S3";
constexpr char kPassword[] = "lift-demo";
constexpr uint16_t kPort = 8765;
constexpr bool kEnableWifi = true;
constexpr bool kEnableSd = true;
constexpr bool kSimulated = true;
constexpr uint32_t kIntervalMs = 40;  // 25 Hz
constexpr char kExercise[] = "squat"; // squat, bench, deadlift, hammer_curl
constexpr char kStartDegrees[] = "";   // e.g. "0.0,0.0" (pitch,roll)
constexpr char kEndDegrees[] = "";     // e.g. "80.0,8.0"; provide both or neither

// Example wiring for an external SPI SD breakout, adjust for your watch.
constexpr int kSdCs = 10;
constexpr int kSdMosi = 11;
constexpr int kSdSck = 12;
constexpr int kSdMiso = 13;

NetworkServer server(kPort);
NetworkClient client;
File recording;
bool apReady = false;
uint32_t sessionStart, lastSample, lastFlush;

struct Orientation {
  float pitch;
  float roll;
};

void setupImu() {
  // Start your watch IMU, mount-axis remap, and gyro/accelerometer fusion.
}

bool readFusedOrientation(Orientation &angles) {
  // Replace with your driver; write degrees to angles.pitch and angles.roll.
  // Return true once per fresh sample. Yaw is deliberately not used.
  return false;
}

bool getOrientation(Orientation &angles, uint32_t elapsed) {
  if (!kSimulated) return readFusedOrientation(angles);

  const float fraction = constrain((float(elapsed % 4400) - 380.0f) / 3420.0f, 0.0f, 1.0f);
  const float wave = sinf(PI * fraction);
  const float phase = wave * wave;
  if (!strcmp(kExercise, "bench")) {
    angles = {73.0f * phase, 26.0f * phase};
  } else if (!strcmp(kExercise, "deadlift")) {
    angles = {48.0f * (1.0f - phase), 4.0f * (1.0f - phase)};
  } else if (!strcmp(kExercise, "hammer_curl")) {
    angles = {65.0f * phase, 70.0f * phase};
  } else {
    angles = {80.0f * phase, 8.0f * phase};
  }
  return true;
}

float wrapAngle(float angle) {
  angle = fmodf(angle, 360.0f);
  if (angle > 180.0f) angle -= 360.0f;
  if (angle < -180.0f) angle += 360.0f;
  return angle;
}

void openRecording() {
  SPI.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
  if (!SD.begin(kSdCs, SPI, 4000000)) {
    Serial.println("SD mount failed. Wi-Fi can still stream.");
    return;
  }
  char path[24];
  for (unsigned number = 1; number <= 9999; ++number) {
    snprintf(path, sizeof(path), "/lift%04u.csv", number);
    if (SD.exists(path)) continue;
    recording = SD.open(path, FILE_WRITE);
    if (recording) {
      recording.printf("# exercise=%s\n", kExercise);
      if (kStartDegrees[0] && kEndDegrees[0]) {
        recording.printf("# start_deg=%s\n# end_deg=%s\n", kStartDegrees, kEndDegrees);
      }
      recording.print("ms,pitch_deg,roll_deg\n");
      recording.flush();
      Serial.printf("Recording %s. Send s over Serial to close the file.\n", path);
    } else {
      Serial.println("Could not create SD log.");
    }
    return;
  }
  Serial.println("All 9999 SD log names are in use.");
}

void setup() {
  Serial.begin(115200);
  if (!kSimulated) setupImu();
  if (kEnableSd) openRecording();
  if (kEnableWifi) {
    WiFi.mode(WIFI_AP);
    const IPAddress address(192, 168, 4, 1);
    if (WiFi.softAPConfig(address, address, IPAddress(255, 255, 255, 0)) &&
        WiFi.softAP(kSsid, kPassword)) {
      server.begin();
      apReady = true;
      Serial.printf("Join %s; TCP %s:%u\n", kSsid, WiFi.softAPIP().toString().c_str(), unsigned(kPort));
    } else {
      Serial.println("Wi-Fi access point failed. SD recording can still run.");
    }
  }
  if (kSimulated) Serial.println("Simulated motion enabled.");
  sessionStart = lastSample = lastFlush = millis();
}

void loop() {
  if (Serial.available() && Serial.read() == 's' && recording) {
    recording.close();
    Serial.println("Recording closed. You can remove the microSD card.");
  }
  if (apReady && !client.connected()) {
    client.stop();
    client = server.accept();
    if (client) client.setNoDelay(true);
  }
  const uint32_t now = millis();
  if (now - lastSample >= kIntervalMs) {
    lastSample = now;
    const uint32_t elapsed = now - sessionStart;
    Orientation angles;
    if (getOrientation(angles, elapsed) && isfinite(angles.pitch) && isfinite(angles.roll)) {
      char row[64];
      const int written = snprintf(row, sizeof(row), "%lu,%+.3f,%+.3f\n",
                                   (unsigned long)elapsed, wrapAngle(angles.pitch), wrapAngle(angles.roll));
      if (written > 0 && written < int(sizeof(row))) {
        if (recording && recording.write(reinterpret_cast<const uint8_t *>(row), written) != size_t(written)) {
          recording.close();
          Serial.println("SD write failed. Recording stopped.");
        }
        if (client.connected()) {
          char line[68];
          memcpy(line, "IMU,", 4);
          memcpy(line + 4, row, written);
          if (client.write(reinterpret_cast<const uint8_t *>(line), written + 4) != size_t(written + 4)) {
            client.stop();
          }
        }
      }
    }
  }
  if (recording && now - lastFlush >= 1000) {
    lastFlush = now;
    recording.flush();
  }
  delay(1);
}
