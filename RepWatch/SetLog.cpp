#include "SetLog.h"
#include <Arduino.h>
#include <SD_MMC.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace setlog {
namespace {
enum class Kind { Start, Sample, Finish };
struct Entry {
  Kind kind;
  board::Sample sample;
  motion::Phase phase;
  uint32_t elapsed, repNumber, repMs, first, fastest, interruptedRepMs;
  uint64_t total;
  const char *reason;
  char exerciseName[exerciseNameCapacity];
  uint8_t velocityLoss;
  motion::TrackingSettings tracking;
  motion::PoseGates gates;
};

constexpr char directory[] = "/sets";
QueueHandle_t entries;
std::atomic<bool> pending{false};
std::atomic<Status> state{Status::Idle};
std::atomic<uint32_t> selectedFile{0}, dropped{0};
uint32_t nextNumber = 1;
bool mounted = false;

FILE *openSet(const Entry &entry) {
  if (!mounted) {
    if (!SD_MMC.setPins(2, 1, 3) || !SD_MMC.begin("/sdcard", true)) return nullptr;
    mounted = true;
  }
  if (!SD_MMC.exists(directory) && !SD_MMC.mkdir(directory)) return nullptr;
  for (; nextNumber <= 999999; ++nextNumber) {
    char path[32];
    snprintf(path, sizeof(path), "/sdcard/sets/set%06lu.csv", (unsigned long)nextNumber);
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (descriptor < 0) {
      if (errno == EEXIST) continue;
      return nullptr;
    }
    selectedFile.store(nextNumber++);
    FILE *file = fdopen(descriptor, "w");
    if (!file) {
      close(descriptor);
      return nullptr;
    }
    auto axis = entry.tracking.axis;
    const char *axisName = axis == motion::TrackingAxis::Pitch ? "pitch" :
                           axis == motion::TrackingAxis::Roll ? "roll" : "both";
    bool ok = fprintf(file, "# exercise_name=%s\n# velocity_loss_pct=%u\n# cutoff_basis=rep_duration_increase\n"
                      "# tracking_axis=%s\n# bottom_tolerance_deg=%.3f\n"
                      "# concentric_tolerance_deg=%.3f\n# finish_tolerance_deg=%.3f\n",
                      entry.exerciseName, unsigned(entry.velocityLoss), axisName,
                      entry.gates.bottom, entry.gates.concentric, entry.gates.top) >= 0;
    if (ok && axis != motion::TrackingAxis::Both)
      ok = fprintf(file, "# main_axis_tolerance_deg=%u\n# off_axis_tolerance_deg=%u\n",
                   unsigned(entry.tracking.mainDegrees), unsigned(entry.tracking.offDegrees)) >= 0;
    if (ok && fputs("ms,roll_deg,pitch_deg,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,phase,rep_number,rep_ms\n", file) >= 0)
      return file;
    fclose(file);
    return nullptr;
  }
  return nullptr;
}

bool writeSample(FILE *out, const Entry &row) {
  const auto &v = row.sample;
  const char *phase = row.phase == motion::Phase::Timing ? "concentric" :
                      row.phase == motion::Phase::Armed ? "armed" : "find_bottom";
  return fprintf(out, "%lu,%.3f,%.3f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%s,%lu,%lu\n",
                 (unsigned long)row.elapsed, motion::roll(v.tilt), motion::pitch(v.tilt),
                 v.acceleration.x, v.acceleration.y, v.acceleration.z, v.gyro.x, v.gyro.y, v.gyro.z,
                 phase, (unsigned long)row.repNumber, (unsigned long)row.repMs) >= 0;
}

void writeLoop(void *) {
  FILE *out = nullptr;
  uint32_t lastSync = 0;
  Entry entry;
  while (xQueueReceive(entries, &entry, portMAX_DELAY) == pdTRUE) {
    switch (entry.kind) {
      case Kind::Start:
        out = openSet(entry);
        lastSync = 0;
        if (!out && mounted) {
          SD_MMC.end();
          mounted = false;
        }
        state.store(out ? Status::Recording : selectedFile.load() ? Status::Partial : Status::Unavailable);
        break;
      case Kind::Sample:
        if (out && writeSample(out, entry)) {
          if (entry.elapsed - lastSync < 1000) break;
          lastSync = entry.elapsed;
          if (fflush(out) == 0 && fsync(fileno(out)) == 0) break;
        }
        if (out) {
          fclose(out);
          out = nullptr;
          SD_MMC.end();
          mounted = false;
          state.store(Status::Partial);
        }
        break;
      case Kind::Finish:
        if (out) {
          uint32_t missed = dropped.load();
          double average = entry.repNumber ? double(entry.total) / entry.repNumber : 0;
          bool ok = fprintf(out,
              "# end=%s,elapsed_ms=%lu,reps=%lu,first_ms=%lu,last_ms=%lu,fastest_ms=%lu,mean_ms=%.1f,interrupted_rep_ms=%lu,dropped_samples=%lu\n",
              entry.reason, (unsigned long)entry.elapsed, (unsigned long)entry.repNumber,
              (unsigned long)entry.first, (unsigned long)entry.repMs, (unsigned long)entry.fastest,
              average, (unsigned long)entry.interruptedRepMs, (unsigned long)missed) >= 0 &&
              fflush(out) == 0 && fsync(fileno(out)) == 0;
          if (fclose(out) != 0) ok = false;
          out = nullptr;
          state.store(ok && !missed ? Status::Saved : Status::Partial);
          if (!ok) {
            SD_MMC.end();
            mounted = false;
          }
        }
        pending.store(false);
        break;
    }
  }
  vTaskDelete(nullptr);
}
}

void begin() {
  entries = xQueueCreate(256, sizeof(Entry));
  if (entries && xTaskCreate(writeLoop, "set-log", 6144, nullptr, 1, nullptr) == pdPASS) return;
  if (entries) vQueueDelete(entries);
  entries = nullptr;
  state.store(Status::Unavailable);
}
bool busy() { return pending.load(); }
Status status() { return state.load(); }
uint32_t fileNumber() { return selectedFile.load(); }

bool start(const char *exerciseName, uint8_t velocityLoss,
           motion::TrackingSettings tracking, motion::PoseGates gates) {
  if (!entries || pending.exchange(true)) return false;
  selectedFile.store(0);
  dropped.store(0);
  state.store(Status::Opening);
  Entry entry{};
  entry.kind = Kind::Start;
  snprintf(entry.exerciseName, sizeof(entry.exerciseName), "%s", exerciseName);
  entry.velocityLoss = velocityLoss;
  entry.tracking = tracking;
  entry.gates = gates;
  if (xQueueSend(entries, &entry, 0) == pdTRUE) return true;
  state.store(Status::Unavailable);
  pending.store(false);
  return false;
}

void append(const board::Sample &sample, uint32_t elapsed, motion::Phase phase,
            uint32_t repNumber, uint32_t repMs) {
  Status current = state.load();
  if (current != Status::Opening && current != Status::Recording) return;
  // Keep one slot for Finish even when the card can't keep up.
  if (uxQueueSpacesAvailable(entries) <= 1) {
    ++dropped;
    return;
  }
  Entry entry{};
  entry.kind = Kind::Sample;
  entry.sample = sample;
  entry.phase = phase;
  entry.elapsed = elapsed;
  entry.repNumber = repNumber;
  entry.repMs = repMs;
  if (xQueueSend(entries, &entry, 0) != pdTRUE) ++dropped;
}

void finish(uint32_t elapsed, const char *reason, const motion::RepSet &stats) {
  Entry entry{};
  entry.kind = Kind::Finish;
  entry.elapsed = elapsed;
  entry.reason = reason;
  entry.repNumber = stats.count;
  entry.repMs = stats.last;
  entry.first = stats.first;
  entry.fastest = stats.fastest;
  entry.interruptedRepMs = stats.interruptedRepMs;
  entry.total = stats.total;
  xQueueSend(entries, &entry, 0);
}
}
