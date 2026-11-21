#pragma once
#include "Board.h"
#include <cstddef>

namespace setlog {
constexpr std::size_t exerciseNameCapacity = 27;
enum class Status { Idle, Opening, Recording, Saved, Partial, Unavailable };
void begin();
bool busy();
Status status();
uint32_t fileNumber();
bool start(const char *exerciseName, uint8_t velocityLoss,
           motion::TrackingSettings tracking, motion::PoseGates gates);
void append(const board::Sample &sample, uint32_t elapsed, motion::Phase phase,
            uint32_t repNumber, uint32_t repMs);
void finish(uint32_t elapsed, const char *reason, const motion::RepSet &stats);
}
