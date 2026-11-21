#pragma once
#include "Board.h"

namespace viewerstream {
enum class Status { Off, Starting, Listening, Streaming, Stopping, Error, Unavailable };
bool begin();
Status status();
void toggle();
void submit(const board::Sample &sample);
}
