#include "Audio.h"
#include "src/audio/VoiceData.h"
#include "src/es8311/es8311.h"
#include <Arduino.h>
#include <ESP_I2S.h>
#include <algorithm>

namespace audio {
namespace {
constexpr uint32_t sampleRate = 16000;
constexpr unsigned framesPerBlock = sampleRate / 100;  // 10 ms.
constexpr unsigned leadBlocks = 8, tailBlocks = 10;
constexpr int ampEnable = 46;
I2SClass speaker;
QueueHandle_t requests;
es8311_handle_t codec;
bool ready = false;
bool muted = false;

using voice::Clip;
constexpr Clip clips[] = {
  {voice::calibrate, sizeof(voice::calibrate) - 1},
  {voice::startrangeofmotion, sizeof(voice::startrangeofmotion) - 1},
  {voice::startconcentric, sizeof(voice::startconcentric) - 1},
  {voice::endrangeofmotion, sizeof(voice::endrangeofmotion) - 1},
  {voice::starteccentric, sizeof(voice::starteccentric) - 1},
  {voice::letsgo, sizeof(voice::letsgo) - 1},
  {voice::starttracking, sizeof(voice::starttracking) - 1},
  {voice::settings, sizeof(voice::settings) - 1},
  {voice::notes, sizeof(voice::notes) - 1},
  {voice::timer, sizeof(voice::timer) - 1},
  {voice::help, sizeof(voice::help) - 1}
};
static_assert(sizeof(clips) / sizeof(clips[0]) == unsigned(Cue::Silence));

bool writeSilence(unsigned blocks) {
  static constexpr int16_t silence[2 * framesPerBlock]{};
  for (unsigned i = 0; i < blocks; ++i) {
    if (uxQueueMessagesWaiting(requests) != 0 ||
        speaker.write(reinterpret_cast<const uint8_t *>(silence), sizeof(silence)) != sizeof(silence))
      return false;
  }
  return true;
}

void playLoop(void *) {
  const Clip *request;
  int16_t stereo[2 * framesPerBlock];
  while (xQueueReceive(requests, &request, portMAX_DELAY) == pdTRUE) {
    if (!request) continue;
    digitalWrite(ampEnable, HIGH);
    // Give the amplifier a quiet lead-in so speech does not start during wake-up.
    if (!writeSilence(leadBlocks)) {
      digitalWrite(ampEnable, LOW);
      continue;
    }
    const Clip &clip = *request;
    size_t offset = 0;
    while (offset + 1 < clip.bytes && uxQueueMessagesWaiting(requests) == 0) {
      size_t frames = std::min(size_t(framesPerBlock), (clip.bytes - offset) / sizeof(int16_t));
      for (size_t frame = 0; frame < frames; ++frame) {
        size_t at = offset + frame * sizeof(int16_t);
        uint16_t bits = uint8_t(clip.pcm[at]) | (uint16_t(uint8_t(clip.pcm[at + 1])) << 8);
        stereo[2 * frame] = stereo[2 * frame + 1] = int16_t(bits);
      }
      size_t bytes = frames * 2 * sizeof(int16_t);
      if (speaker.write(reinterpret_cast<const uint8_t *>(stereo), bytes) != bytes) break;
      offset += frames * sizeof(int16_t);
    }
    // Drain the I2S DMA buffer before switching off the amplifier.
    if (offset == clip.bytes) writeSilence(tailBlocks);
    digitalWrite(ampEnable, LOW);
  }
  vTaskDelete(nullptr);
}

void submit(const Clip *clip) {
  if (!ready) return;
  if (muted) clip = nullptr;
  xQueueOverwrite(requests, &clip);
}
}

bool begin() {
  pinMode(ampEnable, OUTPUT);
  digitalWrite(ampEnable, LOW);
  speaker.setPins(41, 45, 40, -1, 16);
  if (!speaker.begin(I2S_MODE_STD, sampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                     I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) return false;
  // Board.cpp calls this before its IMU task starts using the shared I2C bus.
  codec = es8311_create(0, ES8311_ADDRRES_0);
  const es8311_clock_config_t clock{false, false, true, sampleRate * 256, sampleRate};
  bool codecReady = codec &&
      es8311_init(codec, &clock, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) == ESP_OK &&
      es8311_voice_volume_set(codec, defaultVolume, nullptr) == ESP_OK;
  if (codecReady) {
    requests = xQueueCreate(1, sizeof(const Clip *));
    ready = requests && xTaskCreate(playLoop, "voice", 4096, nullptr, 2, nullptr) == pdPASS;
  }
  if (!ready) {
    if (requests) vQueueDelete(requests);
    if (codec) es8311_delete(codec);
    speaker.end();
  }
  return ready;
}

void play(Cue cue) {
  submit(cue == Cue::Silence ? nullptr : &clips[unsigned(cue)]);
}

void playRep(unsigned number) {
  if (number == 0 || number > sizeof(voice::reps) / sizeof(voice::reps[0])) return;
  const Clip &clip = voice::reps[number - 1];
  if (clip.pcm) submit(&clip);
}

bool setVolume(uint8_t percent) {
  if (!ready || percent > 100 || es8311_voice_volume_set(codec, percent, nullptr) != ESP_OK)
    return false;
  muted = percent == 0;
  if (muted) submit(nullptr);
  return true;
}
}
