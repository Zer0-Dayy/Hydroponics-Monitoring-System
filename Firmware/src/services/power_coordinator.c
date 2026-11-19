#include "services/power_coordinator.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_config.h"
#include "board_profile.h"
#include "diagnostics/logger.h"
#include "diagnostics/progress.h"
#include "hal/clock.h"
#include "policy_config.h"
#include "services/acquisition.h"
#include "services/rail_state.h"
#include "transports/wifi_mqtt.h"

#define POWER_TICK_MS 10u          // level sampling while anything is pending
#define POWER_IDLE_TICK_MS 100u    // steady and armed: the ISR wakes us
#define POWER_HAL_WAIT_MS 2000u    // AcqNormal installs the GPIO ISR service on core 1

static StaticTask_t s_tcb;
static StackType_t s_stack[4 * 1024];  // cfgTaskSpec(TASK_ID_POWER_COORDINATOR)->stackBytes
static TaskHandle_t s_task = NULL;

static portMUX_TYPE s_modeMux = portMUX_INITIALIZER_UNLOCKED;
static PowerMode s_mode = POWER_MODE_UNKNOWN;

static PowerModeFsm s_fsm;  // task-private
static volatile bool s_isrArmed;
static volatile uint32_t s_edges;  // ISR count since boot (diagnostics)

static inline bool senseHigh(void) { return digitalRead(BOARD_PIN_EXTERNAL_POWER_SENSE) == HIGH; }

// Core 1 (the GPIO ISR service lives there). The Arduino service is installed
// without ESP_INTR_FLAG_IRAM, so this never runs while the flash cache is off.
// It does the minimum: invalidate the 5 V epoch, mute itself, wake the task.
static void onSenseEdge(void) {
  railStateInvalidateFromIsr();
  gpio_intr_disable((gpio_num_t)BOARD_PIN_EXTERNAL_POWER_SENSE);
  s_isrArmed = false;
  s_edges = s_edges + 1u;
  BaseType_t woken = pdFALSE;
  if (s_task != NULL) vTaskNotifyGiveFromISR(s_task, &woken);
  portYIELD_FROM_ISR(woken);
}

static void setMode(PowerMode m) {
  portENTER_CRITICAL(&s_modeMux);
  const PowerMode old = s_mode;
  s_mode = m;
  portEXIT_CRITICAL(&s_modeMux);
  if (old != m) logPrintf(LOG_LEVEL_INFO, "power: %s -> %s", powerModeName(old), powerModeName(m));
}

static void execute(const PowerModeActions* a) {
  if (a->quiesce != POWER_WORKER_NONE) {
    acquisitionRequestQuiesce(a->quiesceGen);
    logPrintf(LOG_LEVEL_INFO, "power: quiesce %s worker (gen %lu)", a->quiesce == POWER_WORKER_NORMAL ? "normal" : "low",
              (unsigned long)a->quiesceGen);
  }
  if (a->restoreRail) {
    railStateRestore(a->restoreEpoch);  // a newer edge keeps the rail invalid
    const RailState rail = railStateRead();
    if (rail.valid5v) {
      logPrintf(LOG_LEVEL_INFO, "power: 5 V rail valid (epoch %lu)", (unsigned long)rail.epoch);
    } else {
      logPrintf(LOG_LEVEL_INFO, "power: 5 V restore skipped, epoch %lu -> %lu", (unsigned long)a->restoreEpoch,
                (unsigned long)rail.epoch);
    }
  }
  if (a->grant != POWER_WORKER_NONE) {
    const bool normal = a->grant == POWER_WORKER_NORMAL;
    const bool all = normal || !POLICY_POWER_TRANSITIONS_ENABLED;
    acquisitionGrant(a->grant, a->grantGen, all ? SENSOR_MASK_ALL : ACQ_LOW_POWER_SENSORS);
    logPrintf(LOG_LEVEL_INFO, "power: grant %s worker (gen %lu)", normal ? "normal" : "low",
              (unsigned long)a->grantGen);
  }
  if (a->wifiStop) wifiMqttRequestStop();
  if (a->wifiStart) wifiMqttRequestStart();
}

// After a mode change the edge storm is over once the level has held for the
// debounce time; only then may the interrupt fire again.
static void maybeRearm(bool high, bool* lastHigh, MonoMs* sinceMs, MonoMs now) {
  if (high != *lastHigh) {
    *lastHigh = high;
    *sinceMs = now;
    return;
  }
  if (s_isrArmed || now - *sinceMs < CFG_POWER_LOSS_DEBOUNCE_MS) return;
  s_isrArmed = true;
  gpio_intr_enable((gpio_num_t)BOARD_PIN_EXTERNAL_POWER_SENSE);  // on the service's core (1)
}

static bool armIsr(void) {
  // The service must already exist on core 1, or attachInterrupt() would
  // install it on this core (0) and move the HC-SR04 echo ISR next to the radios.
  for (uint32_t waited = 0; !acquisitionHalsReady(); waited += POWER_TICK_MS) {
    if (waited >= POWER_HAL_WAIT_MS) return false;
    vTaskDelay(pdMS_TO_TICKS(POWER_TICK_MS));
  }
  attachInterrupt(digitalPinToInterrupt(BOARD_PIN_EXTERNAL_POWER_SENSE), onSenseEdge, CHANGE);
  s_isrArmed = true;
  return true;
}

static void powerTask(void* arg) {
  (void)arg;
  if (esp_task_wdt_add(NULL) != ESP_OK) logPrintf(LOG_LEVEL_ERROR, "power: task watchdog add failed");
  bool isr = false;
  if (POLICY_POWER_TRANSITIONS_ENABLED) {
    isr = armIsr();
    if (!isr) logPrintf(LOG_LEVEL_ERROR, "power: GPIO ISR service not ready - polling the sense pin only");
  }
  const bool polling = POLICY_POWER_TRANSITIONS_ENABLED && !isr;
  bool lastHigh = senseHigh();
  bool lastPolled = lastHigh;
  MonoMs sinceMs = monoNowMs();
  for (;;) {
    const MonoMs now = monoNowMs();
    const bool high = senseHigh();
    // Without the ISR, a level change seen here is the edge: invalidate the
    // rail as the ISR would (later than the ISR, by up to one tick).
    if (polling && high != lastPolled) railStateInvalidate();
    lastPolled = high;
    const RailState rail = railStateRead();  // one coherent {valid, epoch} per tick
    const PowerModeInputs in = {now, high, rail.valid5v, acquisitionReleasedGeneration(), wifiMqttRadioOff(),
                                rail.epoch, isr && !s_isrArmed};
    PowerModeActions a;
    powerModeStep(&s_fsm, &in, &a);
    execute(&a);
    setMode(s_fsm.mode);
    if (isr) maybeRearm(high, &lastHigh, &sinceMs, now);

    progressMark(TASK_ID_POWER_COORDINATOR);
    esp_task_wdt_reset();
    const bool busy = s_fsm.mode != POWER_MODE_NORMAL && s_fsm.mode != POWER_MODE_LOW_POWER;
    const bool settling = !high || !in.railValid || (isr && !s_isrArmed) || polling;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(busy || settling ? POWER_TICK_MS : POWER_IDLE_TICK_MS));
  }
}

bool powerCoordinatorInit(void) {
  if (s_task != NULL) return true;
  pinMode(BOARD_PIN_EXTERNAL_POWER_SENSE, INPUT);  // no pull: the 440 Ohm leg defines LOW
  const PowerModeConfig cfg = {CFG_POWER_LOSS_DEBOUNCE_MS, CFG_POWER_RETURN_STABLE_MS, CFG_TRANSITION_ACK_MS,
                               CFG_NETWORK_SHUTDOWN_ACK_MS, POLICY_POWER_TRANSITIONS_ENABLED != 0};
  powerModeInit(&s_fsm, &cfg);

  const TaskSpec* spec = cfgTaskSpec(TASK_ID_POWER_COORDINATOR);
  if (spec->stackBytes != sizeof(s_stack)) {
    logPrintf(LOG_LEVEL_WARN, "power: stack %u B, task table says %lu B", (unsigned)sizeof(s_stack),
              (unsigned long)spec->stackBytes);
  }
  s_task = xTaskCreateStaticPinnedToCore(powerTask, spec->name, sizeof(s_stack), NULL, spec->priority, s_stack,
                                         &s_tcb, spec->core);
  if (s_task == NULL) {
    // Never leave sensing without an owner: run as before WP9.
    railStateRestore(railStateRead().epoch);
    acquisitionGrant(POWER_WORKER_NORMAL, 1, SENSOR_MASK_ALL);
    return false;
  }
  logPrintf(LOG_LEVEL_INFO, "power: started (core %u, prio %u), external power %s, transitions %s",
            (unsigned)spec->core, (unsigned)spec->priority, senseHigh() ? "present" : "ABSENT",
            POLICY_POWER_TRANSITIONS_ENABLED ? "on" : "OFF (kill switch)");
  return true;
}

PowerMode powerModeCurrent(void) {
  portENTER_CRITICAL(&s_modeMux);
  const PowerMode m = s_mode;
  portEXIT_CRITICAL(&s_modeMux);
  return m;
}

void* powerCoordinatorTaskHandle(void) { return s_task; }
