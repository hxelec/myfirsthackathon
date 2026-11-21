#pragma once
#include <cmath>
#include <cstdint>

namespace motion {
constexpr float radians = 0.01745329252f;
constexpr float minimumPoseSeparation = 8.0f;
constexpr float maxStartPoseDegrees = 10.0f;
constexpr float maxEndPoseDegrees = 16.0f;
constexpr float stillDegrees = 1.5f;
constexpr uint32_t restMs = 30000;
constexpr uint32_t maxSetReps = 20;

struct Vec {
  float x, y, z;
  Vec operator+(Vec b) const { return {x + b.x, y + b.y, z + b.z}; }
  Vec operator-(Vec b) const { return {x - b.x, y - b.y, z - b.z}; }
  Vec operator*(float k) const { return {x * k, y * k, z * k}; }
  float dot(Vec b) const { return x * b.x + y * b.y + z * b.z; }
  Vec cross(Vec b) const { return {y*b.z-z*b.y, z*b.x-x*b.z, x*b.y-y*b.x}; }
  float norm() const { return std::sqrt(dot(*this)); }
  Vec unit() const { const float n = norm(); return n > 0 ? *this * (1/n) : Vec{}; }
};

inline bool near(Vec a, Vec b, float degrees) {
  return a.dot(b) >= std::cos(degrees * radians);
}
inline float roll(Vec v) { return std::atan2(v.y, v.z) / radians; }
inline float pitch(Vec v) { return std::atan2(-v.x, std::hypot(v.y, v.z)) / radians; }
inline float tiltDistance(Vec a, Vec b) {
  return std::acos(std::fmax(-1.0f, std::fmin(1.0f, a.dot(b)))) / radians;
}

// Integrate gravity in the sensor frame, then correct with the accelerometer.
// This representation is continuous across +/-180 degrees of displayed roll.
class TiltFilter {
 public:
  static constexpr unsigned startupSamples = 100;
  Vec down{};
  unsigned calibrationSamples() const { return biasSamples; }

  bool update(Vec acc, Vec gyroDps, float dt) {
    float g = acc.norm();
    bool gravityValid = std::fabs(g - 1.0f) < 0.15f;
    if (biasSamples < startupSamples) {
      // The raw rate includes the offset we're measuring. Use changes in
      // rate and tilt to check stillness, with room for factory bias.
      constexpr float maxStartupRate = 20.0f;  // Per axis, degrees/s.
      constexpr float maxRateChange = 1.5f;
      if (!gravityValid || dt > 0.05f ||
          std::fabs(gyroDps.x) > maxStartupRate || std::fabs(gyroDps.y) > maxStartupRate ||
          std::fabs(gyroDps.z) > maxStartupRate ||
          (biasSamples && (!near(biasAnchor, acc.unit(), 1.0f) ||
                           (gyroDps - gyroAnchor).norm() > maxRateChange))) {
        biasSamples = 0;
        bias = {};
        return false;
      }
      if (!biasSamples) {
        biasAnchor = acc.unit();
        gyroAnchor = gyroDps;
      }
      bias = bias + gyroDps;
      if (++biasSamples < startupSamples) return false;
      bias = bias * (1.0f / startupSamples);
      down = acc.unit();
      return true;
    }
    if (dt > 0.05f || down.norm() < 0.5f) {
      down = gravityValid ? acc.unit() : Vec{};
      return gravityValid;
    }
    Vec predicted = (down + down.cross(gyroDps - bias) * (dt * radians)).unit();
    float blend = gravityValid ? dt / (0.55f + dt) : 0.0f;
    down = (predicted * (1 - blend) + acc.unit() * blend).unit();
    return true;
  }
 private:
  Vec bias{}, biasAnchor{}, gyroAnchor{};
  unsigned biasSamples = 0;
};

struct Capture {
  Vec sum{}, anchor{};
  unsigned count = 0;
  bool stable = true;
  void add(Vec v) {
    if (!count) anchor = v;
    stable = stable && near(v, anchor, 3.0f);
    sum = sum + v;
    ++count;
  }
};

enum Pose { Bottom, Concentric, Top, Eccentric, PoseCount };
enum class Phase { FindBottom, Armed, Timing };
enum class TrackingAxis : uint8_t { Both, Pitch, Roll };
struct TrackingSettings {
  constexpr TrackingSettings(TrackingAxis trackingAxis = TrackingAxis::Both, uint8_t main = 10, uint8_t off = 20)
      : axis(trackingAxis), mainDegrees(main), offDegrees(off) {}
  TrackingAxis axis;
  uint8_t mainDegrees, offDegrees;
};

inline float axisAngle(Vec v, TrackingAxis axis) {
  return axis == TrackingAxis::Pitch ? pitch(v) : roll(v);
}

inline TrackingAxis otherAxis(TrackingAxis axis) {
  return axis == TrackingAxis::Pitch ? TrackingAxis::Roll : TrackingAxis::Pitch;
}

inline float angleDelta(float a, float b) { return std::remainder(a - b, 360.0f); }

inline float poseDistance(Vec a, Vec b, TrackingAxis axis) {
  if (axis == TrackingAxis::Both) return tiltDistance(a, b);
  return std::fabs(angleDelta(axisAngle(a, axis), axisAngle(b, axis)));
}

struct PoseGates {
  float bottom, concentric, top;
};

inline PoseGates poseGates(const Vec poses[PoseCount], TrackingSettings settings = {}) {
  TrackingAxis axis = settings.axis;
  float bottomStart = poseDistance(poses[Bottom], poses[Concentric], axis);
  float bottomEnd = poseDistance(poses[Bottom], poses[Top], axis);
  float startEnd = poseDistance(poses[Concentric], poses[Top], axis);
  float maxStart = axis == TrackingAxis::Both ? maxStartPoseDegrees : settings.mainDegrees;
  float bottom = std::fmin(maxStart, 0.49f * std::fmin(bottomStart, bottomEnd));
  float start = std::fmin(maxStart, 0.49f * std::fmin(bottomStart, startEnd));
  // Keep the wider finish region clear of both start gates, including in older calibrations.
  float maxEnd = axis == TrackingAxis::Both ? maxEndPoseDegrees : settings.mainDegrees;
  float end = std::fmin(maxEnd, std::fmin(bottomEnd - bottom, startEnd - start) - 0.1f);
  return {bottom, start, end};
}

// A continuous hold is relative to a fixed orientation. Slow drift accumulates
// instead of disappearing into tiny differences between adjacent samples.
struct Hold {
  Vec anchor{};
  uint32_t since = 0;
  bool tracking = false;
  uint32_t elapsed(Vec v, uint32_t now) {
    if (!tracking || !near(anchor, v, stillDegrees)) {
      anchor = v;
      since = now;
      tracking = true;
    }
    return now - since;
  }
};

struct RepSet {
  RepSet() = default;
  explicit RepSet(const Vec poses[PoseCount], TrackingAxis trackingAxis = TrackingAxis::Both)
      : RepSet(poses, TrackingSettings{trackingAxis, 10, 20}) {}
  RepSet(const Vec poses[PoseCount], TrackingSettings settings)
      : axis(settings.axis), offDegrees(settings.offDegrees), gates(poseGates(poses, settings)),
        startAxis((poses[Concentric] - poses[Bottom]).unit()), startPlane(poses[Concentric].dot(startAxis)) {
    if (axis != TrackingAxis::Both) {
      startAngle = axisAngle(poses[Concentric], axis);
      direction = angleDelta(startAngle, axisAngle(poses[Bottom], axis)) < 0 ? -1.0f : 1.0f;
    }
  }

  Phase phase = Phase::FindBottom;
  uint32_t started = 0, first = 0, last = 0, count = 0, fastest = 0;
  uint32_t interruptedRepMs = 0;
  uint64_t total = 0;
  uint8_t velocityLoss = 100;
  bool slowRep = false, rested = false;
  Hold bottomHold{};
  PoseGates tolerances() const { return gates; }

  // Seed movement at the countdown boundary without timing or counting any reps.
  void prime(Vec v) {
    previous = v;
    havePrevious = true;
    approachLow = approachHigh = startProgress(v);
  }

  void sample(Vec v, uint32_t now, const Vec poses[PoseCount]) {
    if (slowRep || rested || count >= maxSetReps) return;
    bool bottom = matches(v, poses[Bottom], gates.bottom);
    bool top = matches(v, poses[Top], gates.top);
    bool start = approachingStart(v, poses[Concentric]);
    if (phase == Phase::Timing && count > 0) {
      uint32_t elapsed = now - started;
      slowRep = uint64_t(elapsed) * 100 >= uint64_t(first) * (100 + velocityLoss);
      if (slowRep && !top) {
        interruptedRepMs = elapsed;
        return;
      }
    }
    if (bottom) {
      rested = bottomHold.elapsed(v, now) > restMs;
    } else {
      bottomHold.tracking = false;
    }
    if (rested) return;

    if (bottom) {
      phase = Phase::Armed;  // Also discard an incomplete bottom-to-bottom attempt.
    } else if (!top && start && (phase == Phase::Armed || (phase == Phase::FindBottom && !count))) {
      phase = Phase::Timing;
      started = now;
    } else if (top) {
      if (phase == Phase::Timing) {
        last = now - started;
        total += last;
        if (++count == 1) {
          first = last;
          fastest = last;
        } else if (last < fastest) {
          fastest = last;
        }
      }
      phase = Phase::FindBottom;
    }
  }

 private:
  TrackingAxis axis = TrackingAxis::Both;
  uint8_t offDegrees = 20;
  PoseGates gates{};
  Vec startAxis{}, previous{};
  float startPlane = 0, startAngle = 0, direction = 1, approachLow = 0, approachHigh = 0;
  bool havePrevious = false;

  bool matches(Vec v, Vec pose, float tolerance) const {
    if (axis == TrackingAxis::Both) return near(v, pose, tolerance);
    return poseDistance(v, pose, axis) <= tolerance &&
           poseDistance(v, pose, otherAxis(axis)) <= offDegrees;
  }

  float startProgress(Vec v) const {
    if (axis == TrackingAxis::Both) return v.dot(startAxis) - startPlane;
    return angleDelta(axisAngle(v, axis), startAngle) * direction * radians;
  }

  bool crossesStart(Vec v, Vec concentric, float fraction) const {
    if (axis == TrackingAxis::Both) {
      Vec crossing = (previous + (v - previous) * fraction).unit();
      return near(crossing, concentric, gates.concentric);
    }
    TrackingAxis secondary = otherAxis(axis);
    float before = axisAngle(previous, secondary);
    float atGate = before + angleDelta(axisAngle(v, secondary), before) * fraction;
    return std::fabs(angleDelta(atGate, axisAngle(concentric, secondary))) <= offDegrees;
  }

  bool approachingStart(Vec v, Vec concentric) {
    constexpr float minimumAdvance = 1.5f * radians;
    if (!havePrevious) {
      prime(v);
      return false;
    }
    float before = startProgress(previous);
    float current = startProgress(v);
    // A roll discontinuity opposite the start pose is outside the tracking region.
    if (axis != TrackingAxis::Both && std::fabs(current - before) > 180.0f * radians) {
      prime(v);
      return false;
    }
    if (current < approachLow || current + minimumAdvance < approachHigh) {
      approachLow = approachHigh = current;
    } else {
      approachHigh = std::fmax(approachHigh, current);
    }
    bool advanced = current > before && current - approachLow >= minimumAdvance;
    bool crossed = false;
    if (advanced && before < 0 && current >= 0) {
      // A fast movement can cross the gate between readings. Check where its path crossed.
      crossed = crossesStart(v, concentric, -before / (current - before));
    }
    previous = v;
    return advanced && (crossed || matches(v, concentric, gates.concentric));
  }
};
}  // namespace motion
