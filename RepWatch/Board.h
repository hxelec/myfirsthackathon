#pragma once
#include "Motion.h"

namespace board {
struct Sample {
  uint32_t ms;
  motion::Vec tilt;
  motion::Vec acceleration;
  motion::Vec gyro;
};
const char *begin();
bool startImu();
bool pop(Sample &sample);
// Returns -1 when raw IMU data is absent or stale.
int imuCalibrationProgress();
// Returns false for a failed transaction. Zero contacts is a valid release.
bool touch(bool &pressed, int &x, int &y);
int batteryPercent();
bool speakerVolume(uint8_t percent);
void motor(bool on);
}
