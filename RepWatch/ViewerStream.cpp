#include "ViewerStream.h"
#include <Arduino.h>
#include <Network.h>
#include <WiFi.h>
#include <atomic>
#include <lwip/sockets.h>

namespace viewerstream {
namespace {
constexpr char ssid[] = "LiftWatch-S3";
constexpr char password[] = "lift-demo";
constexpr uint16_t port = 8765;
constexpr uint32_t intervalMs = 20;
const IPAddress address(192, 168, 4, 1);
QueueHandle_t latest = nullptr;
TaskHandle_t worker = nullptr;
std::atomic<Status> state{Status::Unavailable};

bool running(Status current) {
  return current == Status::Listening || current == Status::Streaming;
}

bool sendSample(NetworkClient &client, const board::Sample &sample) {
  const auto &a = sample.acceleration;
  const auto &g = sample.gyro;
  char line[128];
  const int length = snprintf(line, sizeof(line),
      "IMU6,%lu,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f\n",
      (unsigned long)sample.ms, a.x, a.y, a.z, g.x, g.y, g.z);
  if (length <= 0 || length >= int(sizeof(line))) return true;
  // A partial line would break framing, so close the connection and let the viewer reconnect.
  return ::send(client.fd(), line, length, MSG_DONTWAIT) == length;
}

void networkLoop(void *) {
  for (;;) {
    if (state.load() != Status::Starting) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    WiFi.persistent(false);
    NetworkServer server(address, port, 1);
    bool started = WiFi.mode(WIFI_AP) &&
                   WiFi.softAPConfig(address, address, IPAddress(255, 255, 255, 0)) &&
                   WiFi.softAP(ssid, password, 1, 0, 1);
    if (started) {
      server.begin();
      started = bool(server);
    }
    if (!started) {
      server.end();
      WiFi.mode(WIFI_OFF);
      state.store(Status::Error);
      continue;
    }

    xQueueReset(latest);
    state.store(Status::Listening);
    NetworkClient client;
    while (running(state.load())) {
      if (!client.connected()) {
        client.stop();
        client = server.accept();
        if (client) client.setNoDelay(true);
      }
      board::Sample sample{};
      if (xQueueReceive(latest, &sample, pdMS_TO_TICKS(intervalMs)) == pdTRUE &&
          running(state.load()) && client && !sendSample(client, sample))
        client.stop();
      // A UI request to stop takes precedence over connection status.
      Status current = state.load();
      if (running(current))
        state.compare_exchange_strong(current, client ? Status::Streaming : Status::Listening);
    }
    client.stop();
    server.end();
    state.store(WiFi.mode(WIFI_OFF) ? Status::Off : Status::Error);
  }
}
}  // namespace

bool begin() {
  if (worker) return true;
  latest = xQueueCreate(1, sizeof(board::Sample));
  if (!latest) return false;
  if (xTaskCreate(networkLoop, "viewer-stream", 6144, nullptr, 1, &worker) != pdPASS) {
    vQueueDelete(latest);
    latest = nullptr;
    return false;
  }
  state.store(Status::Off);
  return true;
}

Status status() { return state.load(); }

void toggle() {
  Status current = state.load();
  for (;;) {
    Status next;
    if (current == Status::Off || current == Status::Error) next = Status::Starting;
    else if (running(current)) next = Status::Stopping;
    else return;
    if (state.compare_exchange_strong(current, next)) {
      xTaskNotifyGive(worker);
      return;
    }
  }
}

void submit(const board::Sample &sample) {
  static bool sent = false;
  static uint32_t previous = 0;
  if (!running(state.load()) || (sent && sample.ms - previous < intervalMs)) return;
  previous = sample.ms;
  sent = true;
  xQueueOverwrite(latest, &sample);
}
}
