#pragma once

// Add ViewerStream.h and ViewerStream.cpp to the existing RepWatch sketch.
#include "Board.h"

namespace viewerstream {
bool begin();
void submit(const board::Sample &sample);
}
