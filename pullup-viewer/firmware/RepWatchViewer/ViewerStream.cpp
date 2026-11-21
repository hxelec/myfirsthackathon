#include "ViewerStream.h"

#include <WiFi.h>
#include <Network.h>
#include <lwip/sockets.h>

namespace viewerstream {
namespace {
constexpr char ssid[] = "LiftWatch-S3";
constexpr char password[] = "lift-demo";
constexpr uint16_t port = 8765;
constexpr uint32_t intervalMs = 20;
QueueHandle_t latest = nullptr;

void networkLoop(void *) {
  NetworkServer server(port);
  NetworkClient client;
  server.begin();
  for (;;) {
    if (!client.connected()) {
      client.stop();
      client = server.accept();
      if (client) client.setNoDelay(true);
    }

    board::Sample sample{};
    if (xQueueReceive(latest, &sample, pdMS_TO_TICKS(20)) != pdTRUE || !client) continue;
    const auto &a = sample.acceleration;
    const auto &g = sample.gyro;
    char line[128];
    const int length = snprintf(line, sizeof(line),
        "IMU6,%lu,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f\n",
        (unsigned long)sample.ms, a.x, a.y, a.z, g.x, g.y, g.z);
    if (length <= 0 || length >= int(sizeof(line))) continue;
    // Keep radio backpressure away from the watch UI and the IMU sampling task.
    const int sent = ::send(client.fd(), line, length, MSG_DONTWAIT);
    if (sent != length) client.stop();
  }
}
}

bool begin() {
  if (latest) return true;
  latest = xQueueCreate(1, sizeof(board::Sample));
  if (!latest) return false;
  const IPAddress address(192, 168, 4, 1);
  if (WiFi.mode(WIFI_AP) && WiFi.softAPConfig(address, address, IPAddress(255, 255, 255, 0)) &&
      WiFi.softAP(ssid, password) &&
      xTaskCreate(networkLoop, "viewer-stream", 4096, nullptr, 1, nullptr) == pdPASS) return true;
  WiFi.softAPdisconnect(true);
  vQueueDelete(latest);
  latest = nullptr;
  return false;
}

void submit(const board::Sample &sample) {
  static bool sent = false;
  static uint32_t previous = 0;
  if (!latest || (sent && sample.ms - previous < intervalMs)) return;
  previous = sample.ms;
  sent = true;
  xQueueOverwrite(latest, &sample);
}
}
