// Production entry point - bootstrap ONLY (Firmware Architecture §5).
// No sensing, reconnect or policy work belongs here or in loop().
// Order (RTOS Architecture §5): safe outputs, logger, flash gate, board gate,
// queues, Configuration (publishes the applied config that Safety and
// acquisition read on their first pass), Safety BEFORE acquisition (the
// first alarm event always has a receiver), PowerCoordinator (grants the
// acquisition lease), the BLE transport, Wi-Fi, then Supervisor. A failed step is logged and boot continues; the relay is
// already de-energised and only Safety can change it.
#include <Arduino.h>

#include "board_profile.h"
#include "diagnostics/logger.h"
#include "hal/arduino_entry.h"
#include "hal/gpio_safe.h"
#include "services/acquisition.h"
#include "services/configuration.h"
#include "services/flash_gate.h"
#include "services/power_coordinator.h"
#include "services/safety.h"
#include "services/snapshot_store.h"
#include "services/supervisor.h"
#include "transports/ble_transport.h"
#include "transports/wifi_mqtt.h"

void setup(void) {
  // 1. Safe output defaults before anything else.
  gpioApplySafeDefaults();

  // 2. Serial console: UART0 at 115200 (CONFIG_ESP_CONSOLE_UART_BAUDRATE).
  logInit();
  logPrintf(LOG_LEVEL_INFO, "hydroponics firmware - stage 6 (BLE app link, calibration, low-power mode)");
  if (!flashGateInit()) logPrintf(LOG_LEVEL_ERROR, "flash gate init failed - pings unguarded");

  // 3. Board gate: pins assigned and power sense measured.
  BoardValidation board = boardProfileValidate();
  logPrintf(LOG_LEVEL_INFO, "board gate: power sense %s, relay %s -> %s",
            board.powerSenseAssigned ? "yes" : "NO",
            board.relayAssigned ? "yes" : "NO",
            board.productionReady ? "pass" : "FAIL");

  // 4. Static queues.
  if (!snapshotStoreInit()) logPrintf(LOG_LEVEL_ERROR, "snapshot store init failed");

  // 5. Configuration: loads NVS and publishes before the tasks below start.
  // On failure the compiled defaults apply (all sensors on).
  if (!configurationInit()) logPrintf(LOG_LEVEL_ERROR, "configuration init failed - compiled defaults");

  // 6-10. Tasks. Both acquisition workers start without the lease;
  // PowerCoordinator grants the first one from the sense level (WP9).
  if (!safetyInit()) logPrintf(LOG_LEVEL_ERROR, "safety init failed - relay stays OFF");
  if (!acquisitionInit()) logPrintf(LOG_LEVEL_ERROR, "acquisition init failed");
  if (!powerCoordinatorInit()) logPrintf(LOG_LEVEL_ERROR, "power coordinator init failed - normal mode, no transitions");
  // Sensing and Safety run without it: a failed radio only costs the app link.
  if (!bleTransportInit()) logPrintf(LOG_LEVEL_ERROR, "BLE transport init failed - no app link");
  // Never waits for an access point; without credentials it stays idle.
  if (!wifiMqttInit()) logPrintf(LOG_LEVEL_ERROR, "Wi-Fi init failed - Bluetooth only");
  if (!supervisorInit()) logPrintf(LOG_LEVEL_ERROR, "supervisor init failed");
}

void loop(void) {
  // Compatibility stub: the Arduino loop task blocks forever.
  vTaskDelay(portMAX_DELAY);
}
