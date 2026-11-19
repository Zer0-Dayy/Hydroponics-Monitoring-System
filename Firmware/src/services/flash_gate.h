// Flash-write gate: keeps NVS commits out of the HC-SR04 echo window.
// Work package WP8 (Stage 5). RTOS Architecture content_2 §227.
//
// A flash write or erase disables the cache on both cores, which delays the
// echo-edge interrupt on core 1 and corrupts the distance. Acquisition holds
// the gate for the length of one ping; Configuration holds it around each
// NVS commit.
//
// Rules:
// - Acquisition only ever TRIES (zero wait) and defers the ping if the gate
//   is busy. It never blocks on Configuration.
// - Configuration waits at most a bounded time, then retries later.
// - Created by flashGateInit() at the very start of boot, independent of
//   Configuration. Before that (bring-up builds never call it), every take
//   succeeds: there is no writer to keep out.
// Mutex owned by one task at a time; take and give from the same task.
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool flashGateInit(void);

bool flashGateTryTakeForPing(void);       // zero wait (acquisition)
void flashGateGiveAfterPing(void);        // safe to call when not held

bool flashGateTakeForWrite(uint32_t waitMs);  // bounded wait (Configuration)
void flashGateGiveAfterWrite(void);
