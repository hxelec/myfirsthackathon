#include "Board.h"
#include "Audio.h"
#include <Arduino.h>
#include <Wire.h>
#include <atomic>

namespace board {
namespace {
constexpr uint8_t pmu = 0x34, touchIC = 0x38, motorPin = 18;
uint8_t imu = 0x6B;
QueueHandle_t samples;
SemaphoreHandle_t wireLock;
std::atomic<uint32_t> lastRawMs{0};
std::atomic<unsigned> stillSamples{0};

bool readReg(uint8_t device, uint8_t reg, uint8_t *dst, size_t size) {
  if (xSemaphoreTake(wireLock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  Wire.beginTransmission(device);
  Wire.write(reg);
  bool ok = Wire.endTransmission(false) == 0 && Wire.requestFrom(device, size) == size &&
            Wire.readBytes(dst, size) == size;
  xSemaphoreGive(wireLock);
  return ok;
}
bool writeReg(uint8_t device, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(device);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}
bool changePmu(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t old;
  return readReg(pmu, reg, &old, 1) && writeReg(pmu, reg, (old & ~mask) | value);
}
int16_t little16(const uint8_t *p) {
  return static_cast<int16_t>(p[0] | (uint16_t(p[1]) << 8));
}
motion::Vec axes(const uint8_t *bytes, float scale) {
  return {little16(bytes) / scale, little16(bytes+2) / scale, little16(bytes+4) / scale};
}
void sampleLoop(void *) {
  motion::TiltFilter filter;
  uint32_t previousUs = micros(), lastStamp = 0x1000000;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
    // Read sensor timestamp, temperature and both XYZ triples in one burst.
    uint8_t data[17];
    if (!readReg(imu, 0x30, data, sizeof(data))) continue;
    uint32_t stamp = data[0] | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16);
    if (stamp == lastStamp) continue;
    lastStamp = stamp;
    uint32_t nowUs = micros();
    float dt = (nowUs - previousUs) * 1e-6f;
    previousUs = nowUs;
    uint32_t nowMs = millis();
    motion::Vec acceleration = axes(data+5, 8192);
    motion::Vec gyro = axes(data+11, 64);
    bool tiltReady = filter.update(acceleration, gyro, dt);
    stillSamples.store(filter.calibrationSamples(), std::memory_order_relaxed);
    lastRawMs.store(nowMs, std::memory_order_release);
    if (tiltReady) {
      Sample sample{nowMs, filter.down, acceleration, gyro};
      // A queue overflow creates a timestamp gap, handled by the set controller.
      xQueueSend(samples, &sample, 0);
    }
  }
}
}  // namespace

const char *begin() {
  pinMode(motorPin, OUTPUT);
  motor(false);
  if (!Wire.begin(15, 14, 400000)) return "I2C setup failed";
  wireLock = xSemaphoreCreateMutex();
  if (!wireLock) return "I2C lock failed";
  Wire.setTimeOut(15);
  // ALDO3 -> R7 -> motor pad P1. P2 is switched by Q1 on GPIO18.
  // 3.0 V for a nominal 3 V motor, preserving other LDO settings.
  if (!changePmu(0x94, 0x1F, 25) || !changePmu(0x90, 0x04, 0x04))
    return "Motor power setup failed";
  changePmu(0x68, 1, 1);  // Battery detection
  changePmu(0x30, 1, 1);  // Battery voltage ADC

  pinMode(9, OUTPUT);
  digitalWrite(9, LOW);
  delay(20);
  digitalWrite(9, HIGH);
  delay(80);
  if (!writeReg(touchIC, 0xA5, 0)) return "Touch setup failed";

  uint8_t id;
  if (!readReg(imu, 0, &id, 1) || id != 5) imu = 0x6A;
  if (!readReg(imu, 0, &id, 1) || id != 5) return "IMU not found";
  if (!writeReg(imu, 0x08, 0) || !writeReg(imu, 0x02, 0x40) ||
      !writeReg(imu, 0x03, 0x15) || !writeReg(imu, 0x04, 0x55) ||
      !writeReg(imu, 0x06, 0x77) || !writeReg(imu, 0x08, 0x03))
    return "IMU setup failed";
  // +/-4 g, +/-512 degrees/s, 224.2 Hz paired output, ~30 Hz low-pass.
  delay(200);  // Let the gyro settle before collecting the bias window.
  if (!audio::begin()) Serial.println("Speaker unavailable");
  return nullptr;
}
bool startImu() {
  samples = xQueueCreate(64, sizeof(Sample));
  return samples && xTaskCreate(sampleLoop, "tilt", 4096, nullptr, 3, nullptr) == pdPASS;
}
bool pop(Sample &sample) {
  return samples && xQueueReceive(samples, &sample, 0) == pdTRUE;
}
int imuCalibrationProgress() {
  uint32_t last = lastRawMs.load(std::memory_order_acquire);
  if (last == 0 || millis() - last > 250) return -1;
  return int(stillSamples.load(std::memory_order_relaxed));
}
bool touch(bool &pressed, int &x, int &y) {
  uint8_t data[5];
  if (!readReg(touchIC, 0x02, data, sizeof(data))) return false;
  pressed = (data[0] & 15) != 0;
  x = ((data[1] & 15) << 8) | data[2];
  y = ((data[3] & 15) << 8) | data[4];
  return true;
}
int batteryPercent() {
  uint8_t status, percent;
  if (!readReg(pmu, 0, &status, 1) || !(status & 8) ||
      !readReg(pmu, 0xA4, &percent, 1) || percent > 100) return -1;
  return percent;
}
bool speakerVolume(uint8_t percent) {
  if (xSemaphoreTake(wireLock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  bool ok = audio::setVolume(percent);
  xSemaphoreGive(wireLock);
  return ok;
}
void motor(bool on) { digitalWrite(motorPin, on ? HIGH : LOW); }
}  // namespace board
