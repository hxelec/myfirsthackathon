# Merge Wi-Fi telemetry into RepWatch

This add-on is for the existing Arduino RepWatch sketch on the Waveshare ESP32-S3-Touch-AMOLED-2.06. Paste this document into the Arduino chat along with the current RepWatch sketch. The two source files are included below and belong next to `RepWatch.ino`.

In `Watch.cpp`, add this alongside the other includes.

```cpp
#include "ViewerStream.h"
```

In `watchBegin()`, initialize streaming after `setlog::begin()`.

```cpp
if (!viewerstream::begin()) Serial.println("Viewer Wi-Fi unavailable");
```

In `watchUpdate()`, replace the existing `board::pop()` loop with this loop. Pass the same sample to the stream and the existing rep logic.

```cpp
while (board::pop(sample)) {
  viewerstream::submit(sample);
  acceptSample(sample);
}
```

The add-on uses the existing QMI8658C reader and its `board::Sample` fields `ms`, `acceleration`, and `gyro`. Keep RepWatch's existing `Board.cpp` and SD logger in charge of those peripherals. Preserve `# exercise_name=...` and `# velocity_loss_pct=...` at the top of set files. The desktop uses that metadata during CSV playback and the exercise dropdown for live streaming.

After boot and watch IMU warm-up, join **LiftWatch-S3** using **lift-demo**. The watch serves one TCP client at **192.168.4.1:8765**. Each fresh line contains boot-relative milliseconds, acceleration in g, and gyro in degrees per second. The stream targets up to 50 Hz. The watch queues the latest sample so a slow network client can't stall the watch UI. A dropped packet leaves a timestamp gap the desktop can see.

```text
IMU6,2040,0.03000,-0.14000,0.98000,1.2500,-3.5000,0.4200
```

Use the board and compiler settings in the existing RepWatch README, with **Tools → Flash Size → 32MB** and **Tools → Partition Scheme → 32M Flash (4.8MB APP/22MB FATFS)**. The current merged sketch exceeds the default and Minimal SPIFFS app partitions. This setting applies to the watch's internal flash. Set CSVs still go to the microSD card.

Open Lift Studio, select the exercise, click **Live**, and capture both endpoint poses. To open a card file, wait for **Saved** on the watch, then select the CSV in the viewer. [Waveshare board documentation](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.06)

## ViewerStream.h

```cpp
#pragma once

// Add ViewerStream.h and ViewerStream.cpp to the existing RepWatch sketch.
#include "Board.h"

namespace viewerstream {
bool begin();
void submit(const board::Sample &sample);
}
```

## ViewerStream.cpp

```cpp
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
```
