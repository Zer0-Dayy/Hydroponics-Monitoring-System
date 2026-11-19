// PowerCoordinator: power-mode state machine and acquisition lease owner.
// Work package WP9 (Stage 6). Architecture §13, §14, §15.
//
// Task "PowerCoord" (cfgTaskSpec: core 0, priority 5, 4 KiB). The decisions
// live in domain/power_mode (host-tested); this service only feeds it and
// carries out its actions:
//   sense    GPIO8 (external 5 V through 220 / 2 x 220 Ohm, measured 25 Oct
//            2026, BOARD_POWER_SENSE_VERIFIED). The edge ISR runs on core 1
//            (the GPIO ISR service is installed there by AcqNormal for the
//            HC-SR04 echo): it invalidates the rail (rail_state, new epoch),
//            disables its own interrupt (a loss chatters 479-928 edges) and
//            notifies this task. The task samples the level every
//            POWER_TICK_MS and re-arms the interrupt once the level has been
//            quiet for CFG_POWER_LOSS_DEBOUNCE_MS.
//   lease    acquisitionRequestQuiesce / acquisitionReleased / acquisitionGrant
//   network  wifiMqttRequestStop / wifiMqttRadioOff / wifiMqttRequestStart
//   rail     railStateRestore (only this task restores validity)
// Debounce: CFG_POWER_LOSS_DEBOUNCE_MS (50 ms, app_config.h) is the mode
// debounce; BOARD_POWER_SENSE_QUALIFY_MS (20 ms) was the bench qualification
// window and is not used here. The measured bounce is 5-14 ms.
//
// Safety stays on core 1 in every mode (D14, 28 Oct 2026): unlike §15, core 1
// is not idle in low power (Safety and SerialLogger keep running there; only
// AcqNormal is blocked). CPU frequency and light sleep are unchanged (§15:
// later decision, measure first).
//
// POLICY_POWER_TRANSITIONS_ENABLED 0 (policy_config.h) is the kill switch:
// no ISR, every sensor on the normal worker, the rail kept valid, and the
// mode only reports the debounced sense level.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "domain/power_mode.h"

// Starts the task. On failure the normal worker is granted every sensor and
// the rail is restored, so sensing runs as before WP9.
bool powerCoordinatorInit(void);

PowerMode powerModeCurrent(void);  // zero wait, any task

// For Supervisor. NULL before init.
void* powerCoordinatorTaskHandle(void);
