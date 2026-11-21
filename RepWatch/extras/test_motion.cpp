// Desktop regression tests. Compile with a C++11 (or newer) compiler.
#include "../Motion.h"
#include "../WorkoutTimer.h"
#include <cassert>
#include <cstdio>

using namespace motion;

static Vec tilt(float degrees) {
  return {0, std::sin(degrees * radians), std::cos(degrees * radians)};
}

static Vec tilted(float pitchDegrees, float rollDegrees) {
  Vec v = tilt(rollDegrees) * std::cos(pitchDegrees * radians);
  v.x = -std::sin(pitchDegrees * radians);
  return v;
}

int main() {
  const Vec poses[] = {tilt(0), tilt(20), tilt(80), tilt(75)};
  RepSet s(poses);
  s.velocityLoss = 20;
  auto visit = [&](float angle, uint32_t ms) { s.sample(tilt(angle), ms, poses); };
  visit(20, 10); visit(80, 20);
  assert(!s.count && s.phase == Phase::FindBottom);
  visit(0, 30); visit(80, 40); visit(20, 50); // Skipped gate then descending.
  assert(s.phase == Phase::FindBottom);
  visit(0, 60); visit(20, 70); visit(0, 80); // Aborted partial.
  assert(!s.count && s.phase == Phase::Armed);
  visit(20, 100); visit(80, 1100);
  assert(s.count == 1 && s.first == 1000 && !s.slowRep);
  visit(80, 1200); visit(20, 1300); visit(80, 1400);
  assert(s.count == 1);
  visit(0, 1500); visit(20, 1600); visit(80, 2799);
  assert(s.count == 2 && s.last == 1199 && !s.slowRep);
  visit(0, 3200); visit(20, 3300); visit(60, 4499);
  assert(s.count == 2 && s.phase == Phase::Timing && !s.slowRep);
  RepSet reachesTop = s;
  reachesTop.sample(poses[Top], 4500, poses);
  assert(reachesTop.slowRep && reachesTop.count == 3 && reachesTop.last == 1200 &&
         reachesTop.interruptedRepMs == 0);
  visit(60, 4500);
  assert(s.slowRep && s.interruptedRepMs == 1200);
  assert(s.count == 2 && s.first == 1000 && s.last == 1199 && s.total == 2199 && s.fastest == 1000);
  visit(0, 5000); visit(20, 5100); visit(80, 5500);
  assert(s.count == 2 && s.last == 1199);

  RepSet late(poses);
  late.prime(tilt(19.9f));  // The countdown ends with the watch already in the start region.
  late.sample(tilt(20.1f), 5000, poses);
  assert(late.phase == Phase::FindBottom);
  late.sample(tilt(24), 5010, poses);
  assert(late.phase == Phase::Timing && late.started == 5010);
  late.sample(tilt(69), 5400, poses);
  assert(late.count == 1);
  late.sample(tilt(15), 5500, poses);
  late.sample(tilt(25), 5600, poses);
  late.sample(tilt(80), 5900, poses);
  assert(late.count == 1);  // Later reps still need to approach the bottom.

  RepSet crossing(poses);
  crossing.prime(tilt(10));
  crossing.sample(tilt(34), 5000, poses);  // Neither sample is inside the start gate.
  assert(crossing.phase == Phase::Timing && crossing.started == 5000);

  RepSet descending(poses);
  descending.prime(tilt(80));
  descending.sample(tilt(25), 5010, poses);
  descending.sample(tilt(15), 5020, poses);
  assert(descending.phase == Phase::FindBottom);
  descending.sample(tilt(24), 5030, poses);
  assert(descending.phase == Phase::Timing);

  const uint8_t losses[] = {40, 60, 80, 100};
  const uint32_t cutoffs[] = {1400, 1600, 1800, 2000};
  for (unsigned i = 0; i < 4; ++i) {
    RepSet trial(poses);
    trial.velocityLoss = losses[i];
    trial.sample(poses[Bottom], 0, poses);
    trial.sample(poses[Concentric], 100, poses);
    trial.sample(poses[Top], 1100, poses);
    trial.sample(poses[Bottom], 1200, poses);
    trial.sample(poses[Concentric], 1300, poses);
    trial.sample(tilt(60), 1300 + cutoffs[i] - 1, poses);
    assert(!trial.slowRep);
    trial.sample(tilt(60), 1300 + cutoffs[i], poses);
    assert(trial.slowRep && trial.interruptedRepMs == cutoffs[i]);
  }

  RepSet rest(poses);
  rest.sample(tilt(0), 7000, poses);
  rest.sample(tilt(10), 36000, poses);
  rest.sample(tilt(0), 37000, poses);
  rest.sample(tilt(0), 67000, poses);
  assert(!rest.rested);
  rest.sample(tilt(0), 67001, poses);
  assert(rest.rested);

  RepSet full(poses);
  uint32_t ms = 0;
  for (uint32_t rep = 1; rep <= maxSetReps; ++rep) {
    full.sample(poses[Bottom], ms, poses);
    full.sample(poses[Concentric], ms + 100, poses);
    full.sample(poses[Top], ms + 1100, poses);
    assert(full.count == rep && !full.slowRep);
    ms += 1500;
  }
  full.sample(poses[Bottom], ms, poses);
  full.sample(poses[Concentric], ms + 100, poses);
  full.sample(poses[Top], ms + 1100, poses);
  assert(full.count == 20 && full.total == 20000 && full.phase == Phase::FindBottom);

  RepSet wideEnd(poses);
  const PoseGates wide = poseGates(poses);
  assert(wide.bottom > 9 && wide.concentric > 9 && wide.top == 16);
  wideEnd.sample(poses[Bottom], 0, poses);
  wideEnd.sample(tilt(73), 50, poses); // Skipped the start gate.
  wideEnd.sample(poses[Concentric], 100, poses);
  assert(wideEnd.phase == Phase::FindBottom && wideEnd.count == 0);
  wideEnd.sample(tilt(8), 150, poses);
  wideEnd.sample(tilt(12), 200, poses);
  wideEnd.sample(tilt(63), 400, poses);
  assert(wideEnd.phase == Phase::Timing);
  wideEnd.sample(tilt(67), 450, poses); // 13 degrees from the calibrated end.
  wideEnd.sample(poses[Eccentric], 500, poses);
  assert(wideEnd.phase == Phase::FindBottom && wideEnd.count == 1 && wideEnd.last == 250);

  const Vec closePoses[] = {tilt(0), tilt(20), tilt(29), tilt(25)};
  const PoseGates close = poseGates(closePoses);
  assert(close.concentric + close.top < 9 && close.bottom + close.concentric < 20);

  const Vec pitchPoses[] = {tilted(0, 0), tilted(30, 0), tilted(65, 0), tilted(60, 0)};
  RepSet pitchSet(pitchPoses, TrackingAxis::Pitch);
  pitchSet.sample(tilted(6, 18), 0, pitchPoses);
  assert(pitchSet.phase == Phase::Armed);
  pitchSet.sample(tilted(25, 19), 100, pitchPoses);
  assert(pitchSet.phase == Phase::Timing);
  pitchSet.sample(tilted(54, 15), 250, pitchPoses);  // Pitch differs by 11 degrees.
  pitchSet.sample(tilted(60, 21), 300, pitchPoses);  // Roll differs by 21 degrees.
  assert(pitchSet.phase == Phase::Timing);
  pitchSet.sample(tilted(60, 19), 350, pitchPoses);
  assert(pitchSet.count == 1 && pitchSet.last == 250);

  const Vec rollPoses[] = {tilted(0, 145), tilted(0, 178), tilted(0, -145), tilted(0, -150)};
  RepSet rollSet(rollPoses, TrackingAxis::Roll);
  rollSet.prime(tilted(15, 167));
  rollSet.sample(tilted(19, -169), 100, rollPoses);  // Jump across the start gate and +/-180.
  assert(rollSet.phase == Phase::Timing);
  rollSet.sample(tilted(19, -151), 400, rollPoses);
  assert(rollSet.count == 1 && rollSet.last == 300);
  assert(std::fabs(poseDistance(tilted(0, 179), tilted(0, -179), TrackingAxis::Roll) - 2) < 0.01f);

  RepSet custom(pitchPoses, TrackingSettings(TrackingAxis::Pitch, 15, 10));
  assert(custom.tolerances().top == 15 && custom.tolerances().bottom < 15);
  custom.sample(tilted(0, 9), 0, pitchPoses);
  custom.sample(tilted(30, 9), 100, pitchPoses);
  custom.sample(tilted(52, 11), 200, pitchPoses);
  assert(custom.phase == Phase::Timing);
  custom.sample(tilted(52, 9), 300, pitchPoses);
  assert(custom.count == 1);

  WorkoutTimer timer;
  timer.adjust(-1);
  assert(timer.minutes() == 1);
  for (int i = 0; i < 10; ++i) timer.adjust(1);
  assert(timer.minutes() == 9 && timer.remaining(0) == 540000);
  for (int i = 0; i < 8; ++i) timer.adjust(-1);
  uint32_t start = UINT32_MAX - 29000;
  timer.start(start);
  timer.stop(start + 10000);
  assert(!timer.running() && timer.remaining(start + 20000) == 50000);
  timer.start(start + 20000);
  uint32_t end = start + 70000;
  assert(!timer.update(end - 1) && timer.remaining(end - 1) == 1);
  assert(timer.update(end) && timer.finished());
  assert(timer.buzz(end) && !timer.buzz(end + 650));
  assert(timer.buzz(end + 950) && !timer.buzz(end + 1600));
  assert(timer.buzz(end + 1900) && !timer.alarming(end + 2550));
  timer.start(end + 3000);
  assert(timer.remaining(end + 3000) == 60000);

  RepSet wrap(poses);
  wrap.sample(poses[Bottom], UINT32_MAX-1100, poses);
  wrap.sample(poses[Concentric], UINT32_MAX-999, poses);
  wrap.sample(poses[Top], 0, poses);
  assert(wrap.first == 1000);
  Capture c;
  for (int i=0; i<300; ++i) c.add(tilt(i%2 ? 179 : -179));
  assert(c.stable && near(c.sum.unit(), tilt(180), 0.1f));
  c.add(tilt(170));
  assert(!c.stable);

  TiltFilter f;
  const Vec bias{0.2f, 0.1f, -0.2f};
  for (int i=0; i<99; ++i) assert(!f.update(tilt(0), bias, .01f));
  assert(f.update(tilt(0), bias, .01f));
  for (int i=1; i<=200; ++i)
    assert(f.update(tilt(i*.9f) * 1.4f, bias + Vec{90,0,0}, .01f));
  assert(near(f.down, tilt(180), 0.2f)); // Gyro-only through a dynamic acceleration.
  assert(!f.update({0,0,0}, bias, .2f));
  assert(f.update({-1,0,0}, bias, .01f));
  assert(std::fabs(pitch(f.down)-90) < .01f);

  TiltFilter cold;
  const Vec coldBias{8.0f, -5.0f, 2.0f};
  auto still = [&](Vec rate) { return cold.update(tilt(0), rate, .01f); };
  for (int i=0; i<60; ++i) assert(!still(coldBias));
  assert(cold.calibrationSamples() == 60);
  assert(!still(coldBias + Vec{0,0,3}));
  assert(cold.calibrationSamples() == 0);
  for (int i=0; i<99; ++i) {
    Vec noise{(i%2 ? 0.12f : -0.12f), 0, 0};
    assert(!still(coldBias + noise));
  }
  assert(still(coldBias));
  for (int i=0; i<100; ++i) assert(cold.update(tilt(0) * 1.3f, coldBias, .01f));
  assert(near(cold.down, tilt(0), .05f)); // The learned bias cancels raw drift.
  std::puts("Passed repetition order, baseline, mid-rep cutoff, stillness, rest, wrap, capture and fusion tests.");
}
