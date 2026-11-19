// Per-task progress timestamps for the progress watchdog.
// Work package WP10 (Stage 4). Architecture §16.
#pragma once
#include "app_config.h"
#include "domain/types.h"

void progressMark(TaskId task);  // call after meaningful work
MonoMs progressLastMs(TaskId task);
