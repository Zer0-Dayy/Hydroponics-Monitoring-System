// Latest-snapshot mailbox (§8, §9). Length-one queue.
// Writer: the acquisition lease holder only (xQueueOverwrite).
// Readers: Safety, BLE, MQTT peek with zero wait - never consume.
#pragma once
#include <stdbool.h>
#include "domain/reading.h"

bool snapshotStoreInit(void);
void snapshotPublish(const Snapshot* snapshot);
bool snapshotPeek(Snapshot* out);  // false if nothing published yet
