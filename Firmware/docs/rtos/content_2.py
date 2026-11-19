# Sections 6-9: acquisition engine, Safety, channels, interrupts.
from rtkit import *
import figs as G
from reportlab.platypus import CondPageBreak


# ---------------------------------------------------------------- 6
def acquisition():
    s = [H1("6. The acquisition engine (AcqNormal)", "r6")]
    s.append(short(
        "One task on core 1 owns all seven drivers. It keeps a table of jobs with absolute deadlines, sleeps until the earliest one, and on each wake advances every due driver by one bounded step. "
        "Slow work – conversions inside a sensor, filter windows of spaced ADC taps – happens while the task is Blocked, so a single task can run all sensors in parallel without threads or locks. "
        "Every completed reading is stamped, stored in the working snapshot, checked against its alarm rules and published at once."))
    s.append(H2("6.1 One worker, one owner (D05, D15)"))
    s.append(P("Until WP9 there is exactly one acquisition task. It constructs every driver and bus HAL (I2C controller 0 on GPIO21/47, 1-Wire on GPIO14, ADC1 channels on GPIO1/6/7, pulse capture on GPIO17/18) with the pins from board_profile.h, and no other task ever calls them. "
               "Because nothing else touches a bus, there is no mutex, no priority inversion, and no possibility of two transactions interleaving on the shared BME280 / SGP30 I2C bus. "
               + m("acquisitionGrant()") + " and " + m("acquisitionRequestQuiesce()") + " stay stubs until WP9 adds the second worker and the lease (section 13)."))
    s.append(H2("6.2 The job table"))
    s.append(P("The schedule is data, not code: a small static table of jobs, each with an absolute next deadline. The table and its pure functions live in " + m("src/domain/schedule.c") + " " + tag("done") + " so they are host-tested; the task only calls them."))
    s.append(table(["Job", "Period / phase", "Driver steps", "Host budget", "Inputs from other sensors"], [
        ["BME280", "250 ms / 0, 250, 500, 750", "start (forced) → poll PENDING to +10 ms → READY", "12 ms elapsed, < 1 ms CPU (11 ms measured)", "–"],
        ["DS18B20", "1 s / first, after the BME280 slot", "start (Convert T) → probe converts 750 ms → poll reads the scratchpad at ≈ +760 ms, same frame", "~7 ms then ~12 ms blocking, separately", "–"],
        ["SGP30", "1 s / after DS18B20", "start (measure_iaq) → PENDING to +13 ms → READY", "15 ms (13 ms measured)", "absolute humidity from a fresh BME280 sample"],
        ["HC-SR04", "1 s / after the 1-Wire start", "start (10 µs trigger) → ISR timestamps → READY / TIMEOUT", "≤ 30 ms timeout", "–"],
        ["TDS", "1 s / after HC-SR04 trigger", "15 ADC taps, 40 ms apart, trimmed mean", "≈ 560 ms elapsed, µs per tap", "latest DS18B20 water temperature (0.2–1.2 s old)"],
        ["pH", "1 s / same", "10 taps, 30 ms apart", "≈ 270 ms elapsed", "–"],
        ["Light", "1 s / same", "10 taps, 11 ms apart", "≈ 99 ms elapsed", "–"],
        ["Frame boundary", "1 s / 0", "record jitter, publish snapshot, re-arm the 1 s jobs", "≤ 10 ms (Rev 3 budget)", "–"],
    ], [0.12, 0.2, 0.3, 0.18, 0.2]))
    s.append(P("A job has three timing fields: " + m("periodMs") + ", " + m("nextDueMs") + " (absolute, monotonic) and " + m("budgetMs") + ". A driver that is PENDING additionally carries the " + m("nextPollMs") + " it returned. The task's next wake-up is the minimum over all of them."))
    s.append(H2("6.3 One second, step by step"))
    s.append(G.frame())
    s.append(caption("Figure 3 – Planned interleave of one frame. Dark = the worker is running; light = the sensor converts on its own; amber ticks = single ADC conversions."))
    s.append(bullets([
        "<b>t = 0.</b> Frame boundary: the worker records how late it woke (release jitter), publishes the snapshot of the previous frame and re-arms the one-second jobs. The BME280 slot is also due, so its forced conversion starts now.",
        "<b>t ≈ 10 ms.</b> BME280 READY (11 ms measured): result stored, rule check, publish.",
        "<b>t ≈ 11–18 ms.</b> DS18B20 start: reset, Match ROM, Convert T (~7 ms blocking). It waits until the BME280 conversion is done, because a blocking call during it would make the BME280 poll late and time out. The probe now converts on its own for 750 ms.",
        "<b>t ≈ 18 ms.</b> SGP30 measure command (with humidity compensation from the fresh BME280 reading); the chip converts while the worker continues.",
        "<b>t ≈ 20 ms.</b> HC-SR04 trigger – after the 1-Wire start, and only if the DS18B20 read is not due within the 30 ms echo window, because 1-Wire slots mask interrupts for up to 70 µs and would delay the echo-edge timestamp (onewire_bus.h).",
        "<b>t ≈ 32 ms.</b> SGP30 READY. <b>t ≈ 50 ms.</b> HC-SR04 READY or TIMEOUT.",
        "<b>t ≈ 50 ms onward.</b> The three analog windows start. Each tap is one ADC1 conversion of a few microseconds; between taps the worker is Blocked. Taps from different channels interleave freely because the worker performs them one at a time.",
        "<b>t = 250, 500, 750 ms.</b> BME280 slots preempt the order <i>between</i> steps: a blocking step whose budget would cross the slot, or that would run while a BME280 conversion is pending, is deferred until after it.",
        "<b>t ≈ 153 / 322 / 610 ms.</b> Light, pH and TDS windows complete; each result is published immediately.",
        "<b>t ≈ 761–773 ms.</b> DS18B20 read (~12 ms blocking), after the 750 ms slot's BME280 is done: CRC check, result published. TDS uses this value in the next frame, so its compensation input is 0.2–1.2 s old (limit 3 s). From then to 1000 ms the worker only idles.",
    ]))
    s.append(note("Refinement of Rev 3 section 9",
        "Rev 3 drew the analog group as 3 × 5 ms of CPU. The drivers written since use spaced filter windows (pH 270 ms, TDS 560 ms, light 99 ms elapsed), exactly as Rev 3's rule 'do not compress a spaced filter window into a burst' asks. "
        "CPU use stays far below the 150 ms reservation; elapsed time is what the schedule must fit, and it fits inside one frame."))
    s.append(H2("6.4 Driving one driver: the DriverRun state machine"))
    s.append(G.driverrun())
    s.append(caption("Figure 4 – Per-driver stepping. Every transition is one bounded driver call."))
    s.append(table(["Situation", "What the engine does", "Reading published"], [
        ["init() fails", "Back off: retry at a later frame (bounded, e.g. every 5 s), never in the same frame.", "STARTING, or FAULT if it worked before"],
        ["start() / poll() returns FAULT", "Reading FAULT; DriverRun → IDLE. Next attempt at the next period.", "FAULT, last-good kept separately"],
        ["poll() returns NOT_PRESENT", "Mark uninitialised; init again at the back-off deadline.", "FAULT (audit L2 makes the DS18B20 do this)"],
        ["Still PENDING at its frame deadline", "cancel(); reading TIMEOUT with kQualityLate.", "TIMEOUT"],
        ["I2C NACK on init", "Only a stuck bus (SDA held LOW) triggers i2cBusRecover(); a missing device just fails (audit L5).", "FAULT"],
        ["Dependency missing", "TDS without fresh water temperature, SGP30 without fresh humidity: the driver reports it.", "DEPENDENCY_INVALID / uncompensated flag"],
    ], [0.25, 0.47, 0.28]))
    s.append(H2("6.5 How the task waits"))
    s.append(G.acq_loop())
    s.append(caption("Figure 5 – AcqNormal's loop. One pass is a few milliseconds; the rest of the time the task is Blocked."))
    s.append(code("for (;;) {\n"
                  "  const MonoMs now0 = monoNowMs();\n"
                  "  const MonoMs due  = scheduleEarliestDue(s_jobs, JOB_COUNT);  // min of all deadlines\n"
                  "  ulTaskNotifyTake(pdTRUE, due > now0 ? pdMS_TO_TICKS(due - now0) : 0);\n"
                  "  const MonoMs now = monoNowMs();\n"
                  "  acqRunDueJobs(now);           // bounded: each job advances one step\n"
                  "  progressMark(TASK_ID_ACQUISITION_NORMAL);\n"
                  "  esp_task_wdt_reset();\n"
                  "}"))
    s.append(H2("6.6 Deadlines: skip, never catch up"))
    s.append(P("When a job runs late – because a blocking DS18B20 call overran, or the task was starved – " + m("scheduleJobMarkRun()") + " advances its deadline by <b>whole periods past now</b>. Missed slots are counted (" + m("late") + ", " + m("skipped") + ") and reported by Supervisor; they are never replayed in a burst. "
               "A burst would put several readings milliseconds apart, add no information and push every other deadline further out."))
    s.append(example("A 30 ms overrun",
        "The BME280 slot is due at 500 ms. The DS18B20 reset takes longer than budgeted and the worker reaches the slot at 530 ms. The BME280 runs once, at 530 ms, flagged late (30 ms). "
        "Its next deadline is 750 ms – not 780 ms, and not an extra catch-up run – so the 250 ms grid is kept. Supervisor's next line shows " + m("late=1") + "."))
    s.append(H2("6.7 After every completed reading"))
    s.append(table(["Step", "Detail"], [
        ["1 Read", "sensorDriverReadResult() copies up to 4 Reading records into a local array."],
        ["2 Stamp", "powerEpoch (0 until WP9), configRevision, and the per-metric sequence counter (so readers can tell a new sample from a re-published one)."],
        ["3 Cross-sensor", "BME280 T + RH → sgp30 humidity input; DS18B20 water T → tdsDriverSetWaterTemperature(). The consumer driver checks the age itself."],
        ["4 Store", "s_work.readings[metric] = reading. s_work is a static Snapshot, the only copy acquisition mutates."],
        ["5 Rules", "For each enabled rule on that metric: evaluateRule(rule, &amp;r, now, staleLimit, &amp;state). TRIP or CLEAR → safetySubmitEvent() (zero wait) which also notifies Safety."],
        ["6 Publish", "snapshotPublish(&amp;s_work): xQueueOverwrite copies it; readers see the new value immediately."],
    ], [0.14, 0.86]))
    s.append(why("Publish after every reading, not once per second",
        "A light or temperature value is visible to Safety a few microseconds after it is measured instead of up to a second later. The cost is one 584-byte copy per reading – about a dozen per second."))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 7
def safety():
    s = [H1("7. Safety and the grow-lamp relay", "r7")]
    s.append(short(
        "Safety is a small state machine on core 0 at the highest application priority. It is the only code that writes GPIO15. "
        "It wakes when acquisition reports an alarm event, and in any case every 100 ms. Each wake runs the five steps of §12 in a fixed order, then blocks again. "
        "The grow lamp is its first automatic action: ON in low light, with hysteresis and a minimum dwell, and OFF whenever the light data is stale or faulty or an interlock is latched."))
    s.append(G.safety_loop())
    s.append(caption("Figure 6 – Safety's wake loop. Steps 1 and 4 are placeholders until WP9 and WP8."))
    s.append(H2("7.1 Where the logic lives"))
    s.append(P("The task in " + m("services/safety.c") + " only waits, copies inputs and writes the pin. All decisions are in a pure function, " + m("safetyCoreStep()") + " in " + m("src/domain/safety_core.c") + " " + tag("done") + ":"))
    s.append(code("typedef struct SafetyInputs {\n"
                  "  const Snapshot*   snap;          // NULL until the first publish\n"
                  "  const AlarmEvent* events; uint8_t eventCount;\n"
                  "  MonoMs            now;\n"
                  "} SafetyInputs;\n\n"
                  "typedef struct SafetyOutputs {\n"
                  "  bool       relayOn;       // what relay 1 (GPIO15) must be\n"
                  "  bool       lampWantsOn;   // lamp decision before interlock / enable\n"
                  "  LampReason lampReason;    // level, dwell, fault, stale, interlock, ...\n"
                  "  bool       interlock;\n"
                  "  uint16_t   latchedMask, activeMask, staleMask;  // one bit per rule\n"
                  "} SafetyOutputs;\n\n"
                  "void safetyCoreStep(SafetyCore* self, const SafetyInputs* in, SafetyOutputs* out);"))
    s.append(P("Because it takes " + m("now") + " as an argument, every scenario in this section is a host unit test: build a snapshot, advance a fake clock, check the relay output."))
    s.append(H2("7.2 The lamp policy"))
    s.append(G.lamp())
    s.append(caption("Figure 7 – Hysteresis band and dwell. The lamp changes state only outside the band and only after the minimum time in the current state."))
    s.append(table(["Rule", "Behaviour", "Source"], [
        ["Hysteresis", "ON below POLICY_LAMP_ON_BELOW_MV (40), OFF above POLICY_LAMP_OFF_ABOVE_MV (120), unchanged in between.", "domain/lamp_control " + tag("done")],
        ["Dwell", "A state is held at least POLICY_LAMP_MIN_ON_MS / _MIN_OFF_MS (30 s bench placeholder; minutes in use). Boot counts as a switch to OFF, so a reset loop cannot flash the lamp.", tag("done")],
        ["Saturated", "A clipped reading (kQualitySaturated) counts as bright.", tag("done")],
        ["Faults", "POLICY_LAMP_FAULTS_TO_OFF unusable readings in a row force OFF at once. Counted per new light result (intervalEndMs changed – the FAULT path does not bump sequence), not per wake.", tag("done") + " + " + tag("plan")],
        ["Staleness", "If the newest light reading ended more than CFG_STALE_ORDINARY_MS (3 s) ago, force OFF on this wake – no new sample needed.", tag("plan") + " fixes audit B1"],
        ["Interlock", "A latched alarm whose action is DEENERGIZE on relay 1 forces OFF and wins over the lamp.", tag("plan")],
        ["Enable switch", m("POLICY_LAMP_AUTO_ENABLED") + " (default 1). With 0, Safety still computes and reports the decision but never energises the relay.", tag("plan")],
    ], [0.15, 0.6, 0.25]))
    s.append(note("Why age comes from intervalEndMs",
        "The light driver publishes UNCALIBRATED millivolts. " + m("readingHealthAt()") + " only ages readings whose health is OK, so it would never call an UNCALIBRATED light reading stale. "
        "Safety therefore computes the age itself as " + m("now − reading.intervalEndMs") + ". (Audit D6 also makes " + m("lastGoodMs") + " mean 'last usable result' for every driver.)"))
    s.append(example("Covering the sensor at night",
        "1. The room is dark: light readings ~20 mV arrive once per second. On the first one below 40 mV, lamp_control wants ON, but the boot dwell has 12 s left: " + m("waiting = true") + ".<br/>"
        "2. Twelve seconds later the dwell expires; the next reading (still below 40 mV) switches the decision to ON. Safety writes GPIO15 HIGH; the relay closes NO; the lamp lights.<br/>"
        "3. The lamp light now reaches the sensor: 95 mV – inside the band, no change. (If it read above 120 mV the lamp would turn itself off; the threshold must be set above the 'night + lamp ON' reading.)<br/>"
        "4. Morning: 300 mV, above 120. After the 30 s ON dwell the decision becomes OFF and GPIO15 goes LOW."))
    s.append(example("The light driver stops producing results",
        "The S wire is fine but the ADC driver begins returning FAULT on every window. Five FAULT readings in a row (5 s) force OFF through the fault counter. "
        "If instead AcqNormal itself hangs, no reading arrives at all: on every 100 ms wake Safety sees the light reading's age grow, and once it passes 3 s the lamp is forced OFF – without a single new event. "
        "Supervisor reports the AcqNormal stall at about the same time, and the task watchdog resets the chip after 5 s (section 11)."))
    s.append(note("What firmware cannot detect",
        "If the S wire is pulled off, the 1 µF capacitor decays to ~0 mV, which reads as darkness: the lamp switches ON. No software rule can distinguish this from a real dark room. A pull-down or a plausibility rule (e.g. 0 mV in daytime hours) needs a hardware or requirements decision."))
    s.append(H2("7.3 Alarm rules and latching"))
    s.append(P("Acquisition evaluates rules (section 6.7); Safety latches and acts. A rule (" + m("AlarmRule") + " in domain/alarm.h " + tag("done") + ") has a metric, comparison, trip and clear thresholds (hysteresis), persistence time, severity, latching flag and actuator action. The evaluator gains a time and state argument (audit L1):"))
    s.append(code("RuleDecision evaluateRule(const AlarmRule* rule, const Reading* sample,\n"
                  "                          MonoMs now, uint32_t staleLimitMs, AlarmRuleState* state);\n"
                  "// NOT_ELIGIBLE unless readingIsActionableAt(sample, now, staleLimitMs)\n"
                  "// TRIP  when beyond trip threshold for >= persistMs (0 = first valid sample)\n"
                  "// CLEAR when back past the clear threshold (hysteresis)"))
    s.append(table(["Situation", "Result"], [
        ["Valid sample beyond trip for persistMs", "TRIP event → sticky slot set → Safety latches, applies the action (relay OFF if DEENERGIZE)."],
        ["Later normal sample before Safety ran", "The sticky slot keeps tripped = true and the worst value: an unhandled critical event cannot be erased."],
        ["Valid sample past clear threshold", "CLEAR event. A non-latching rule releases; a latching one stays latched until an explicit reset (WP8 command)."],
        ["Reading not OK or older than its stale limit", "NOT_ELIGIBLE: no trip and no clear. A sensor fault is a fault, not an environmental alarm."],
        ["Power removed later (WP9)", "Does not erase a latched real alarm."],
    ], [0.38, 0.62]))
    s.append(P("Thresholds are not commissioned, so the rule table in " + m("include/policy_config.h") + " ships with its placeholder rules <b>disabled</b>. Tests and the latency measurement enable their own rules."))
    s.append(H2("7.4 Sticky store and detail queue"))
    s.append(P("Two channels carry alarm information from acquisition to Safety, deliberately different:"))
    s.append(table(["Channel", "Content", "When full", "Purpose"], [
        ["Sticky slot (one per rule)", "tripped flag, first and worst value, count, first time. Updated in a portMUX section.", "Cannot be full: one slot per rule, preallocated.", "Guarantees Safety acts on every trip."],
        ["Detail queue (16 × AlarmEvent)", "Full event: rule, decision, the 56-byte Reading.", "Zero-wait send fails; an overflow counter increments.", "History for reporting (WP8 journal). Losing detail never loses the alarm."],
    ], [0.2, 0.33, 0.24, 0.23]))
    s.append(code("bool safetySubmitEvent(const AlarmEvent* e) {     // called from AcqNormal\n"
                  "  portENTER_CRITICAL(&s_stickyMux);\n"
                  "  stickyUpdate(&s_sticky[e->ruleId], e);          // a few instructions\n"
                  "  portEXIT_CRITICAL(&s_stickyMux);\n"
                  "  const bool queued = xQueueSend(s_detail, e, 0) == pdTRUE;\n"
                  "  if (!queued) s_detailOverflow++;                // reported by Supervisor\n"
                  "  xTaskNotifyGive(s_safetyTask);                  // wake Safety now\n"
                  "  return queued;\n"
                  "}"))
    s.append(H2("7.5 Writing the relay"))
    s.append(P(pname("hal/relay.c") + " gives Safety one function, " + m("relayWrite(bool on)") + ", which maps ON to the electrical level using " + m("BOARD_RELAY1_ACTIVE_LOW") + " (false: active-high through the IRLZ44N / transistor stage). Safety writes only when the decided state changes and logs every change. "
               "No other module includes relay.h; a review rule enforces it."))
    s.append(H2("7.6 Safety's own outputs"))
    s.append(P("After each pass Safety overwrites a length-1 " + pname("SafetyStatus") + " mailbox: relay state, lamp decision and reason (dark / bright / dwell / stale / fault / interlock / disabled), latched-rule mask, overflow counter. Supervisor prints it today; BLE and MQTT will peek it in WP8."))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 8
def channels():
    s = [H1("8. Channels between tasks", "r8")]
    s.append(short(
        "Every piece of data that crosses a task boundary goes through exactly one of the channels below. Each has one writer, a fixed capacity and a defined behaviour when it is full. No task ever passes another a pointer into its own memory."))
    s.append(G.channels())
    s.append(caption("Figure 8 – Stage 4 data paths. Dashed = later readers or loss counting."))
    s.append(table(["Channel", "Writer → reader", "Capacity", "API", "When full / slow reader", "Status"], [
        ["Snapshot store", "AcqNormal → Safety, Supervisor (BLE, MQTT later)", "1 × 584 B", "xQueueOverwrite / xQueuePeek(0)", "Overwrite. Readers may skip versions by design.", tag("done")],
        ["Sticky alarm slots", "AcqNormal → Safety", "1 per rule", "portMUX section", "Never full. Keeps first / worst / count.", tag("plan")],
        ["Alarm detail queue", "AcqNormal → Safety", "16 × AlarmEvent", "xQueueSend(0) / Receive(0)", "Drop and count overflow; sticky state unaffected.", tag("plan")],
        ["Safety wake", "AcqNormal → Safety", "counting notification", "xTaskNotifyGive / ulTaskNotifyTake", "Counts accumulate; one wake drains all.", tag("plan")],
        ["SafetyStatus", "Safety → Supervisor (transports later)", "1 record", "xQueueOverwrite / Peek(0)", "Overwrite.", tag("plan")],
        ["Log queue", "any task → SerialLogger", "32 × 129 B", "xQueueSend(0) / Receive(∞)", "Drop and count; the caller never blocks.", tag("plan")],
        ["Progress stamps", "each task → Supervisor", "1 × 8 B per task", "portMUX section", "Overwrite.", tag("plan")],
        ["Rail state", "power ISR → all", "{valid, epoch}", "portMUX (ISR variant)", "–", tag("done") + " record, " + tag("later") + " ISR"],
        ["Command ingress, journal, lease", "transports / PowerCoordinator", "8 / 64 / 1", "queues", "Reject busy / drop oldest / one grant", tag("later")],
    ], [0.14, 0.18, 0.1, 0.18, 0.22, 0.18]))
    s.append(H2("Invariants a reviewer checks"))
    s.append(bullets([
        "Every send from AcqNormal or Safety uses a <b>zero</b> timeout. A full channel is counted, never waited on.",
        "Only queues of plain fixed-size structs. No pointers inside queued items, no String, no JSON.",
        "A portMUX section contains assignments only – no call that can block, print or loop over a bus.",
        "One writer per channel. A second writer is a design change, not a bug fix.",
    ]))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 9
def interrupts():
    s = [H1("9. Interrupts", "r9")]
    s.append(short(
        "No sensor can raise an interrupt when its value becomes critical (§6), so alarms come from sampling, not from interrupts. Interrupts are used for timing: the HC-SR04 echo edges today, and the external-power edges in WP9."))
    s.append(table(["Source", "Pin / core", "Handler does", "Then", "Status"], [
        ["HC-SR04 ECHO, CHANGE", "GPIO18, core 1 (installed by AcqNormal)", "IRAM: reads the level by register, stores an esp_timer µs timestamp for the rising and the falling edge.", "AcqNormal's next poll() computes the width. No notification: the poll is already scheduled 1 ms ahead.", tag("done")],
        ["External power sense, CHANGE", "GPIO8, core of the GPIO ISR service (see below)", "IRAM: railStateInvalidateFromIsr() – valid = false, epoch++; disable itself; notify PowerCoordinator.", "20 ms qualification in the task, then mode change (§13).", tag("later") + " WP9; needs IRAM_ATTR (audit L3)"],
        ["Timer / tick, IPC, radio", "SDK", "Kernel and SDK internals.", "–", "SDK"],
    ], [0.16, 0.17, 0.32, 0.23, 0.12]))
    s.append(H2("Interactions to respect"))
    s.append(bullets([
        "<b>1-Wire masks interrupts.</b> Each bit slot of the bit-banged 1-Wire bus runs with interrupts off for up to ~70 µs on core 1. An echo edge arriving then is timestamped late (~1 cm). The schedule therefore never runs a DS18B20 transaction while an echo is in flight (section 6.3).",
        "<b>Where an ISR runs.</b> In Arduino 2.0.17 the <i>first</i> attachInterrupt call installs the shared GPIO ISR service (gpio_install_isr_service) on the calling task's core, and every later GPIO handler is dispatched from that same interrupt, on that core. AcqNormal attaches the echo ISR, so today all GPIO interrupts run on core 1. "
        "WP9 must therefore decide deliberately: either install the service from core 0 at boot (before acquisition starts) so the power-sense edge is serviced on core 0, or accept core 1 and measure the latency. Verify on hardware either way.",
        "<b>Flash writes delay GPIO interrupts.</b> The service is not IRAM-flagged, so during an NVS or OTA flash write (WP8) GPIO edges wait until the write finishes. An echo measured across a flash write is wrong; NVS writes must not overlap an HC-SR04 ping (Configuration and acquisition are on different cores, so WP8 needs a rule or a measurement here).",
        "<b>Interrupt watchdog.</b> Any ISR or critical section longer than 300 ms resets the chip. Ours are microseconds.",
    ]))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s
