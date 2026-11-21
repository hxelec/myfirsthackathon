#pragma once
#include <cstdint>

namespace audio {
constexpr uint8_t defaultVolume = 65;
enum class Cue { Calibrate, Bottom, Concentric, Top, Eccentric, LetsGo, StartTracking, Settings, Notes, Timer, Help, Silence };
bool begin();
bool setVolume(uint8_t percent);
void play(Cue cue);
void playRep(unsigned number);
}
