#pragma once
#include <cstdint>

class WorkoutTimer {
 public:
  unsigned minutes() const { return durationMs / minuteMs; }
  uint32_t duration() const { return durationMs; }
  bool running() const { return active; }
  bool finished() const { return expired; }
  uint32_t remaining(uint32_t now) const {
    if (!active) return leftMs;
    uint32_t elapsed = now - startedAt;
    return elapsed < leftMs ? leftMs - elapsed : 0;
  }
  void adjust(int change) {
    if (active) return;
    int next = int(minutes()) + change;
    if (next < 1 || next > 9) return;
    durationMs = leftMs = uint32_t(next) * minuteMs;
    expired = false;
  }
  void start(uint32_t now) {
    if (active) return;
    if (!leftMs) leftMs = durationMs;
    startedAt = now;
    expired = false;
    active = true;
  }
  void stop(uint32_t now) {
    if (active) {
      leftMs = remaining(now);
      active = false;
    }
    expired = false;
  }
  bool update(uint32_t now) {
    if (!active || remaining(now)) return false;
    alarmAt = startedAt + leftMs;
    active = false;
    leftMs = 0;
    expired = true;
    return true;
  }
  bool alarming(uint32_t now) const {
    return expired && now - alarmAt < 2 * buzzCycleMs + buzzMs;
  }
  bool buzz(uint32_t now) const {
    return alarming(now) && (now - alarmAt) % buzzCycleMs < buzzMs;
  }

 private:
  static constexpr uint32_t minuteMs = 60000, buzzMs = 650, buzzCycleMs = 950;
  uint32_t durationMs = minuteMs, leftMs = minuteMs, startedAt = 0, alarmAt = 0;
  bool active = false, expired = false;
};
