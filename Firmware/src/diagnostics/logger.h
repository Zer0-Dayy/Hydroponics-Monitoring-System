// Logger: bounded queue to the serial console, drop-and-count, never blocks
// callers. Work package WP10 (Stage 4). Architecture §9, §16.
//
// logPrintf() formats into a fixed record (LOG_TEXT_LEN, longer text is cut)
// and sends it with a zero timeout. The SerialLogger task (core 1, priority
// 1) prints it, so it only runs while acquisition is Blocked. Before the task
// exists (and in bring-up builds, which never call logInit()) logPrintf()
// prints directly. Never call it from an ISR.
#pragma once
#include <stdint.h>

#define LOG_TEXT_LEN 128

typedef unsigned char LogLevel;
enum { LOG_LEVEL_ERROR = 0, LOG_LEVEL_WARN, LOG_LEVEL_INFO, LOG_LEVEL_DEBUG };

void logInit(void);  // creates the queue and the SerialLogger task
void logPrintf(LogLevel level, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));
uint32_t logDroppedCount(void);  // records lost to a full queue since boot
uint32_t logQueueDepth(void);    // records waiting to be printed (0 before logInit)
void* logTaskHandle(void);       // SerialLogger TaskHandle_t, NULL before logInit()
