# Sections 10-17: timing, supervision, memory, failures, later work packages, implementation, tests, rules.
import sys
from pathlib import Path
from rtkit import *
import figs as G
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "architecture"))
import diagrams as AD  # noqa: E402  (Rev 3 figures reused for the WP9 preview)
from reportlab.platypus import CondPageBreak


# ---------------------------------------------------------------- 10
def timing():
    s = [H1("10. Timing, latency and CPU load", "r10")]
    s.append(short(
        "The design targets three numbers from §19: ≤ 5 ms release jitter of the one-second frame, ≤ 10 ms from an accepted sample to Safety's action, and ≤ 300 ms from a sustained over-temperature to that action. "
        "The worst case is dominated by waiting for the next BME280 sample (250 ms); the task-to-task path itself is microseconds."))
    s.append(H2("10.1 From a sample to the relay"))
    s.append(G.core_timeline())
    s.append(caption("Figure 9 – A BME280 sample that trips a rule, zoomed to 245–275 ms. Both cores shown."))
    s.append(table(["Segment", "Mechanism", "Budget", "Expected"], [
        ["Condition → next sample", "BME280 has no alert pin; sampled every 250 ms", "≤ 250 ms", "0–250 ms, uniform"],
        ["Slot release jitter", "AcqNormal wakes from its notify-wait; nothing higher on core 1", "≤ 5 ms", "≈ 1 ms (tick granularity)"],
        ["Conversion + read", "forced mode, oversampling ×1", "≤ 12 ms", "11 ms measured"],
        ["Rule + submit + notify", "pure function, portMUX update, queue copy, xTaskNotifyGive", "–", "a few µs"],
        ["Safety wake on core 0", "priority 6 preempts Supervisor; idle otherwise", "≤ 10 ms incl. action", "&lt; 1 ms without radios"],
        ["Total, sustained condition", "", "≤ 300 ms (277 ms budget)", "≈ 262 ms worst case"],
    ], [0.24, 0.42, 0.16, 0.18]))
    s.append(AD.latency())
    s.append(caption("Figure 10 – Rev 3 latency budget (§6): 250 + 5 + 12 + 10 = 277 ms, accepted at 300 ms."))
    s.append(H2("10.2 Light, lamp and staleness timing"))
    s.append(table(["Path", "Time"], [
        ["New light reading visible to Safety", "≤ 100 ms after publish (next health wake); no event is sent for the lamp."],
        ["Darkness → lamp ON", "light window (≈ 99 ms) + ≤ 100 ms + the remaining dwell (up to LAMP_MIN_OFF_MS)."],
        ["Acquisition stalls → lamp OFF", "3 s stale limit + ≤ 100 ms."],
        ["Light driver faults → lamp OFF", "POLICY_LAMP_FAULTS_TO_OFF readings, i.e. ≈ 5 s with the placeholder value of 5."],
    ], [0.4, 0.6]))
    s.append(H2("10.3 When the radios arrive (D14)"))
    s.append(P("In WP8 the Wi-Fi, lwIP (priority 18) and NimBLE host tasks join core 0 and outrank Safety. Their bursts can delay Safety by more than the 10 ms target. Rev 3 therefore requires the alarm test to be repeated under continuous MQTT publishing and BLE streaming. "
               "<b>Fallback if it fails:</b> run Safety on core 1 in normal mode at a priority above AcqNormal, and hand it to core 0 only in low power using the same quiesce / grant protocol as acquisition."))
    s.append(H2("10.4 CPU load"))
    s.append(P("Per second, AcqNormal performs roughly 4 BME280 cycles, one DS18B20 read + start (≈ 19 ms blocking, the largest item), two SGP30 transactions, one HC-SR04 ping and 35 ADC taps. "
               "Estimated busy time is 25–35 ms per second on core 1 (≈ 3 %), against the 150 ms reservation of §9. Safety runs ten short passes per second. These are estimates to be replaced by a trace (section 16)."))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 11
def diagnostics():
    s = [H1("11. Logging, supervision and watchdogs (WP10)", "r11")]
    s.append(short(
        "Logging becomes a bounded queue drained by its own low-priority task, so a slow UART never delays a sensor or Safety. Each important task stamps a progress time after real work; Supervisor checks those stamps every second and reports a stall before the 5 s task watchdog resets the chip. "
        "Each task feeds the watchdog only for itself."))
    s.append(H2("11.1 Logger"))
    s.append(table(["Aspect", "Design"], [
        ["Record", "{ LogLevel level; char text[LOG_TEXT_LEN = 128]; } – 129 bytes, copied into the queue."],
        ["logPrintf()", "vsnprintf into a local buffer, truncate at 127 characters, xQueueSend with zero wait. On failure increment the drop counter. Never blocks; never called from an ISR."],
        ["Before the task exists", "Falls back to a direct printf, so boot messages before step 7 still appear."],
        ["SerialLogger task", "core 1, priority 1: xQueueReceive(portMAX_DELAY), then printf('[%c] %s'). It only runs when AcqNormal is Blocked."],
        ["Bounds", "Level checked before indexing the tag table (audit D8)."],
        ["Stack note", "vsnprintf with %f uses about 1 KiB of stack in the caller. Safety (3 KiB) and Supervisor log floats; their high-water marks are checked in the acceptance run."],
    ], [0.22, 0.78]))
    s.append(H2("11.2 Progress stamps"))
    s.append(P(m("progressMark(TASK_ID_x)") + " stores " + m("monoNowMs()") + " in a per-task 64-bit slot under a portMUX section (a 64-bit value cannot be written atomically, section 2.6). It is called after <i>meaningful</i> work: AcqNormal after a pass that advanced at least one job, Safety after each completed pass. A task blocked forever stops stamping, even though the CPU and the idle task are fine."))
    s.append(H2("11.3 Supervisor"))
    s.append(table(["Every", "Check", "Action"], [
        ["1 s", "Progress age: Safety > 1 s, AcqNormal > 2 s, SerialLogger > 2 s while its queue is non-empty", "WARN once per episode, set degraded status"],
        ["1 s", "Counters: alarm-detail overflow, log drops, acquisition late / skipped, driver faults", "Log changes only"],
        ["5 s", "Status line: key readings, health, lamp state and reason, jitter min / max", "INFO"],
        ["10 s", "uxTaskGetStackHighWaterMark per task, esp_get_minimum_free_heap_size, largest free internal block", "WARN below 512 B of stack or a falling heap trend"],
        ["boot", "esp_reset_reason()", "INFO; a TASK_WDT or PANIC reason is a WARN"],
    ], [0.08, 0.62, 0.3]))
    s.append(H2("11.4 Task watchdog"))
    s.append(G.watchdogs())
    s.append(caption("Figure 11 – Who feeds what. Each subscribed task resets the TWDT only for itself."))
    s.append(bullets([
        "The Arduino core has already initialised the TWDT (5 s, panic). Tasks only call " + m("esp_task_wdt_add(NULL)") + " once at start and " + m("esp_task_wdt_reset()") + " after meaningful work.",
        "Subscribed in Stage 4: <b>AcqNormal</b> and <b>Safety</b>. Not subscribed: SerialLogger and Supervisor (lowest priority; legitimately starved under load – their stalls are logged, not fatal).",
        "Core 1 starvation is <i>not</i> covered by the SDK (IDLE1 unsubscribed). AcqNormal's own subscription covers the case that matters.",
        "Supervisor never resets the watchdog on behalf of another task: that would hide exactly the stall the watchdog exists to catch (§18).",
        "WP9 adds the rule from §18: an inactive acquisition worker blocks forever by design and must unsubscribe before it blocks, as part of the lease handover.",
    ]))
    s.append(example("Injected acquisition stall",
        "A test build (" + m("-D FW_INJECT_STALL_ACQ") + ") makes AcqNormal block forever after 60 s. At ~61 s Safety's freshness check sees light data older than 3 s → lamp OFF. At ~62 s Supervisor logs " + m("WARN acq: no progress for 2003 ms") + ". "
        "At ~65 s the TWDT expires for AcqNormal → panic → reset. After the reboot Supervisor logs " + m("reset reason: TASK_WDT") + "."))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 12
def memory():
    s = [H1("12. Memory", "r12")]
    s.append(short(
        "Everything the RTOS design needs is allocated statically at boot, in internal SRAM: about 21 KiB for Stage 4. Nothing is allocated after setup(), so heap fragmentation cannot build up over days of running."))
    s.append(table(["Item", "Size", "Where", "Status"], [
        ["Safety stack + TCB", "3 KiB + ~350 B", "static", tag("plan")],
        ["AcqNormal stack + TCB", "4 KiB + ~350 B", "static", tag("plan")],
        ["SerialLogger stack + TCB", "2 KiB + ~350 B", "static", tag("plan")],
        ["Supervisor stack + TCB", "3 KiB + ~350 B", "static", tag("plan")],
        ["Snapshot store", "584 B + queue header", "static", tag("done")],
        ["Working snapshot (AcqNormal)", "584 B", "static", tag("plan")],
        ["Safety snapshot copy, Supervisor snapshot copy", "2 × 584 B", "static (not on the stack)", tag("plan")],
        ["Alarm detail queue", "16 × 60 B ≈ 960 B", "static", tag("plan")],
        ["Log queue", "32 × 129 B ≈ 4.1 KiB", "static", tag("plan")],
        ["Sticky alarm slots, SafetyStatus, progress", "< 0.5 KiB", "static", tag("plan")],
        ["Driver contexts and HAL structs", "existing", "static", tag("done")],
        ["Total added by Stage 4", "≈ 21 KiB", "of ~320 KiB free internal RAM", ""],
    ], [0.4, 0.22, 0.24, 0.14]))
    s.append(P("The production image used 18.9 KB of static RAM before Stage 4 (audit, 25 Oct). Stack sizes are Rev 3 starting budgets: they are confirmed or corrected from the measured high-water marks (section 11.3), keeping at least 25 % headroom. PSRAM is not used for anything in this design."))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 13
def failures():
    s = [H1("13. Failure behaviour", "r13")]
    s.append(short("Every failure has a defined, bounded behaviour: mark the data, keep sensing and Safety running, report, retry later; reset only as a last resort with the reason logged (§18)."))
    s.append(table(["Failure", "Detected by", "Behaviour", "Lamp"], [
        ["Sensor missing at boot", "init() fails", "Reading STARTING / FAULT; init retried at a bounded interval", "unaffected (unless light)"],
        ["Light sensor faulty", "driver FAULT", "Fault counter; after N readings lamp OFF", "OFF"],
        ["Light wire pulled", "not detectable (reads dark)", "Lamp may turn ON – hardware decision needed", "may turn ON"],
        ["I2C bus stuck low", "transaction timeout + SDA low", "Bus recovery once, both I2C readings FAULT meanwhile", "unaffected"],
        ["DS18B20 no presence", "reset without presence", "FAULT written (audit L2); TDS becomes DEPENDENCY_INVALID", "unaffected"],
        ["Deadline overrun", "schedule lateness", "kQualityLate, skip missed slots, count", "unaffected"],
        ["Alarm detail queue full", "send fails", "Overflow counted; sticky slot still triggers Safety", "interlock still applies"],
        ["Log queue full", "send fails", "Drop and count; no task blocks", "unaffected"],
        ["AcqNormal stalls", "progress age; data age", "Lamp OFF at 3 s, WARN at 2 s, TWDT reset at 5 s", "OFF"],
        ["Safety stalls", "progress age", "WARN at 1 s, TWDT reset at 5 s; relay keeps its last level until reset, then OFF", "last state ≤ 5 s"],
        ["Stack overflow", "canary at context switch", "Panic and reset; reason logged on next boot", "OFF after reset"],
        ["Init of a service fails", "returns false", "ERROR logged, boot continues, relay stays OFF", "OFF"],
    ], [0.2, 0.2, 0.42, 0.18]))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 14
def later():
    s = [H1("14. What changes in WP8 and WP9", "r14")]
    s.append(short(
        "Stage 4 is built so that the later work packages add tasks and protocol without rewriting it. WP8 adds readers and a command path; WP9 adds a second acquisition worker on core 0 and moves the sensors between the two with a generation-numbered lease."))
    s.append(H2("14.1 WP8 – transports and configuration"))
    s.append(bullets([
        "WifiMqtt and BleTransport peek the snapshot and SafetyStatus with zero wait; they never call a driver.",
        "Commands go through an 8-entry ingress queue to Configuration (NVS owner) or, for actuation, to Safety. Safety checks interlocks and expiry before applying any of them (§12 step 4).",
        "The alarm detail queue gains a 64-event journal with independent cursors for BLE and MQTT.",
        "The SGP30 baseline and the lamp thresholds become NVS-backed configuration with a revision number (already carried in every Reading).",
        "D14 radio-load test for Safety (section 10.3).",
    ]))
    s.append(H2("14.2 WP9 – power modes and the lease"))
    s.append(P("The external-power sense on GPIO8 (measured 25 Oct) drives the mode machine. The ISR invalidates the 5 V rail and bumps its epoch; PowerCoordinator qualifies the edge for 20 ms and runs the transitions below. Acquisition's notify-wait (section 6.5) is what lets PowerCoordinator interrupt it at once."))
    s.append(AD.power_sm())
    s.append(caption("Figure 12 – Power modes (Rev 3 §13)."))
    s.append(AD.lease_seq())
    s.append(caption("Figure 13 – Lease handover between the two acquisition workers (Rev 3 §14)."))
    s.append(AD.epoch_timeline())
    s.append(caption("Figure 14 – Why each 5 V reading carries the rail epoch it started in (Rev 3 §14)."))
    s.append(P("Prepared in Stage 4 so WP9 needs no rework: the engine and job table are worker-agnostic; every Reading has a powerEpoch field; drivers already implement cancel() and preparePowerOff(); the task watchdog subscription is per task, so a worker can unsubscribe when it releases the lease."))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 15
def implementation():
    s = [H1("15. Implementation plan and file map", "r15")]
    s.append(short("Four steps, each ending in a test: audit fixes, pure domain logic with host tests, the FreeRTOS services on target, then diagnostics. Frozen headers are not edited; new tunables go into a new, non-frozen header."))
    s.append(table(["Step", "Content", "Ends with"], [
        ["1 Audit prerequisites", "L1 evaluateRule signature; L2 DS18B20 NOT_PRESENT → FAULT; L4 safe GPIO defaults; L5 recover I2C only when SDA is low; D1 test fix; D2 boot wording; D3 comment; D6 lastGoodMs meaning (reading.h wording → team-lead review); D7 snapshotReset identity; D8 logger bounds.", "pio test -e native, all envs build"],
        ["2 Domain logic", "alarm.c (hysteresis, persistence, latch), schedule.c (deadlines, skip, BME280 fit), safety_core.c (§12 order, lamp, staleness, interlock), lampControlForceOff(), policy_config.h.", "New host tests pass, 0 warnings"],
        ["3 Services (WP7)", "acquisition.c engine with all drivers, safety.c task + sticky store + detail queue, hal/relay.c, main.c boot order.", "Lamp follows light on the bench"],
        ["4 Diagnostics (WP10)", "logger queue + SerialLogger, progress portMUX, supervisor.c, TWDT subscriptions.", "Stall injection passes"],
    ], [0.18, 0.6, 0.22]))
    s.append(H2("File map"))
    s.append(table(["File", "Role", "Status"], [
        ["include/app_config.h", "Periods, budgets, queue lengths, cfgTaskSpec() – FROZEN", tag("done")],
        ["include/board_profile.h", "Pins, relay polarity, sense gate – FROZEN", tag("done")],
        ["include/policy_config.h", "Lamp thresholds and dwell, auto-enable switch, alarm rule table (disabled)", tag("done")],
        ["src/main.c", "Boot order only", tag("done")],
        ["src/domain/alarm.c/.h", "Rule evaluation, stale limits", tag("done")],
        ["src/domain/schedule.c/.h", "Job table, deadlines, skip / late accounting", tag("done")],
        ["src/domain/safety_core.c/.h", "Pure Safety state machine", tag("done")],
        ["src/domain/lamp_control.c/.h", "Lamp hysteresis and dwell", tag("done")],
        ["src/services/acquisition.c/.h", "AcqNormal task and engine", tag("done")],
        ["src/services/safety.c/.h", "Safety task, sticky store, detail queue, SafetyStatus", tag("done")],
        ["src/services/snapshot_store.c/.h", "Length-1 snapshot queue", tag("done")],
        ["src/services/supervisor.c/.h", "Supervisor task", tag("done")],
        ["src/services/rail_state.c/.h, power_coordinator.c/.h", "Rail epoch, power modes", tag("done") + " record / " + tag("later") + " WP9"],
        ["src/hal/relay.c/.h", "relayWrite(); included by Safety only", tag("done")],
        ["src/hal/gpio_safe.c", "Boot-time safe levels", tag("done")],
        ["src/diagnostics/logger.c, progress.c", "Log queue + SerialLogger, progress stamps", tag("done")],
        ["test/native/test_domain/test_main.c", "Host tests for all domain logic", tag("done") + " + new tests"],
        ["test/bringup/*_main.c", "Per-sensor bring-up envs (unchanged; test_lamp superseded by production)", tag("done")],
    ], [0.38, 0.44, 0.18]))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 16
def acceptance():
    s = [H1("16. How we prove it works", "r16")]
    s.append(short("Host tests prove the logic with injected time; bench runs prove the timing and the relay on real hardware. Each line maps to a row of the §19 acceptance table."))
    s.append(H2("16.1 Host tests (pio test -e native)"))
    s.append(table(["Area", "Test cases"], [
        ["alarm", "trip on one sample with persist 0; trip only after persistMs; no chatter inside hysteresis; stale sample NOT_ELIGIBLE; FAULT NOT_ELIGIBLE; latch survives clear until reset"],
        ["schedule", "BME280 due 4× per 1 s; DS18B20 due once; after a 3 s gap each job runs once and is counted late/skipped (no burst); a 50 ms step is deferred past a BME280 slot 20 ms away"],
        ["safety_core", "boot OFF; dark → ON only after the dwell; stale light (no new sample) → OFF within 3 s + one wake; N FAULT samples → OFF; interlock beats the lamp; later normal sample does not erase an unhandled TRIP; auto disabled → relay never ON"],
        ["existing", "the 23 current tests keep passing; D1 made non-vacuous"],
    ], [0.15, 0.85]))
    s.append(H2("16.2 On the board (pio run -e esp32dev -t upload -t monitor)"))
    s.append(table(["Test (§19 row)", "Procedure", "Pass when"], [
        ["Build", "pio run for esp32dev and all 11 test_* envs", "all SUCCESS, RAM / flash recorded in README"],
        ["One-second schedule", "Supervisor logs frame release jitter min / max for 30 min", "max ≤ 5 ms; every metric updates ~1 s, BME280 ~250 ms"],
        ["Temperature alarm", "Test build enables a rule that trips at the current air temperature; log sample-end and Safety-action times at varied phases", "sample → action ≤ 10 ms; condition → action ≤ 300 ms"],
        ["Lamp", "Low-voltage load on COM/NO first: cover sensor; torch; hold inside the band; reset", "ON after dwell; OFF after dwell; no chatter; OFF through boot"],
        ["Stall injection", "FW_INJECT_STALL_ACQ, then one with a stalled Safety", "lamp OFF ≤ 3.1 s; WARN; TWDT reset; reason logged"],
        ["Queue overload", "Test rule that trips every sample with a 1-entry detail queue", "overflow counted, Safety still latches every time"],
        ["Endurance", "24 h run", "no unexpected reset, flat heap and stack high-water marks, counters plausible"],
    ], [0.18, 0.47, 0.35]))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 17
def team_rules():
    s = [H1("17. Rules for everyone writing firmware", "r17")]
    s.append(short("A checklist for code review. Breaking one of these is a design change, discussed first – not a shortcut."))
    s.append(table(["Do", "Do not"], [
        ["Call drivers and bus HALs only from the acquisition engine.", "Read a sensor from Safety, a transport, a test hook or loop()."],
        ["Write GPIO15 only through relayWrite() in Safety.", "Include hal/relay.h anywhere else, or drive the pin from a bring-up main in production."],
        ["Block on a notification or queue with a bounded timeout.", "vTaskDelay(1000) in a production task, busy-wait, or poll a flag in a loop."],
        ["Send with a zero timeout and count failures.", "Wait on a full queue from AcqNormal or Safety."],
        ["Pass fixed-size structs by value through queues.", "Pass a pointer to a stack buffer, a String or a JSON object to another task."],
        ["Protect cross-core multi-field updates with portMUX; keep the section to a few assignments.", "Rely on volatile for cross-core consistency, or call anything that can block inside a critical section."],
        ["Put decisions in src/domain with now as a parameter, and test them on the host.", "Include Arduino.h or FreeRTOS headers in src/domain."],
        ["Allocate tasks, queues and buffers statically at boot.", "Call malloc after setup(); keep a Snapshot on a small task stack."],
        ["Use monoNowMs() for every age and deadline.", "Use millis() differences across tasks, tick counts as time, or wall-clock time."],
        ["Log with logPrintf() (non-blocking).", "Call printf from production tasks or from an ISR."],
        ["Reset the task watchdog only from the task itself, after real work.", "Feed the watchdog for another task, or from a timer."],
        ["Take task names, cores, priorities and stacks from cfgTaskSpec().", "Hard-code a priority or core in xTaskCreate."],
        ["Report missing data as a Health value.", "Publish 0 for a value you do not have."],
    ], [0.5, 0.5], first_bold=False))
    s.append(H2("Glossary"))
    s.append(table(["Term", "Meaning here"], [
        ["Frame", "The one-second acquisition cycle that starts at each whole second of the schedule."],
        ["Slot", "One BME280 run of the 250 ms fast lane."],
        ["Job / DriverRun", "A schedule entry for one driver and its init → start → poll stepping state."],
        ["Snapshot", "The latest copy of all readings, 584 B, published by acquisition, peeked by readers."],
        ["Sticky slot", "Per-rule alarm record that cannot be lost when queues are full."],
        ["Stale", "A reading older than its limit: 1 s for the BME280, 3 s for the others."],
        ["Dwell", "Minimum time the lamp stays in a state after a change."],
        ["Interlock", "A latched alarm whose action overrides normal control (relay forced OFF)."],
        ["Progress stamp", "Last time a task did meaningful work; read by Supervisor."],
        ["Epoch / lease", "WP9: rail-change counter in each 5 V reading; exclusive right of one acquisition worker to the buses."],
    ], [0.2, 0.8]))
    return s
