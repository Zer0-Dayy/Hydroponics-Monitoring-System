# Sections 9-15.
from kit import *
import diagrams as G
from reportlab.platypus import PageBreak, CondPageBreak


def schedule():
    s = [H1("9. One-second schedule and timing budget", "s9")]
    s.append(short("Every second the active worker runs a frame: collect the DS18B20 result and start the next conversion, then the SGP30, HC-SR04, the analog sensors and bookkeeping, with a BME280 read slotted in every 250 ms. Planned work is about 138 ms per second (150 ms reserved), so the CPU is free most of the time. Slow conversions run inside the sensors while the worker does other things."))
    s.append(P("The active acquisition worker maintains absolute deadlines using a monotonic clock. It sleeps until the next due event or control notification. vTaskDelayUntil() suits a simple periodic bring-up loop; the integrated multi-rate engine uses a timed notification wait so power changes can wake it immediately. "
               "It never runs a one-second delay that hides intervening 250 ms jobs."))
    s.append(G.timing())
    s.append(caption("Figure 5 – One frame. Dark bars occupy the worker; light bars are conversions running inside the sensor."))
    s.append(H2("Example steady-state sequence"))
    s.append(bullets([
        "At each one-second boundary, read the DS18B20 conversion started in the previous frame and start its next conversion.",
        "Service due BME280 conversion/start/read steps.",
        "Then the SGP30 measurement (it needs its own steady 1 s cadence), HC-SR04, pH, TDS and light.",
        "The BME280 deadline can interrupt this order between bounded operations.",
        "A DS18B20 result is absent during the initial conversion; mark it WARMING, never zero.",
    ], num=True))
    s.append(table(["Work", "Proposed budget / period", "Meaning"], [
        ["BME280 forced conversion + read", "12 ms elapsed, every 250 ms", "9.3 ms maximum conversion at oversampling ×1/×1/×1 from the datasheet; time out if not ready."],
        ["DS18B20 read, CRC and next request", "20 ms host budget / 1 s", "Up to 750 ms conversion at 12 bits runs without occupying the worker."],
        ["SGP30 measurement + read", "15 ms host budget / 1 s", "Send the measure command, let the sensor convert while the worker serves other steps, then read 6 bytes and check both CRCs. Confirm the conversion time against the datasheet [E13]."],
        ["HC-SR04", "30 ms capture timeout / 1 s", "Use RMT or an audited edge-capture adapter. No echo is TIMEOUT, not zero distance."],
        ["Three analog channel groups (pH, TDS, light)", "5 ms each per frame initially", "CPU/ADC budget only; filter window and analog settling are separate."],
        ["Publish snapshot / policy / bookkeeping", "10 ms per frame", "Bounded records; no JSON or serial formatting here."],
        ["Normal frame reservation", "150 ms per second", "48 + 20 + 15 + 30 + 15 + 10 = 138 ms, rounded up (48 = 4 BME280 slots × 12 ms). Not measured WCET or CPU utilization."],
    ], [0.27, 0.23, 0.5], hl3_rows=(2, 6)))
    s.append(H2("Rules inside the frame"))
    s.append(bullets([
        "Serial transaction access remains single-owner even while conversions are pending.",
        "Each service step should finish within 5 ms normally, with documented, bounded timeout exceptions.",
        "Reserve enough time before the next BME280 deadline; defer an ordinary operation that would cross it.",
        "Do not retry a failing sensor repeatedly inside the same frame.",
        "The BME280 and SGP30 share one I2C bus. Their transactions are serialized by the single owner; a BME280 read may run while the SGP30 converts. Check on a logic analyzer that the SGP30 never holds the bus during conversion.",
        "TDS/pH filter taps may be scheduled as small additional ADC substeps. A five-millisecond CPU budget does not justify compressing a manufacturer's spaced filter window into an instantaneous burst. Start with valid sample counts, record window age, and verify noise/latency before fixing the filter configuration. [L8–L12]",
    ]))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def datamodel():
    s = [H1("10. Data model, ownership and freshness", "s10")]
    s.append(short("Every value travels with its quality, its age and the power epoch it was measured in. Consumers never guess: a value is OK, STALE, FAULT, UNPOWERED, WARMING or UNCALIBRATED, and only OK values drive automatic actions."))
    s.append(H2("Reading contract"))
    s.append(P("Use a fixed-size record per metric containing:"))
    s.append(table(["Field", "Purpose"], [
        ["sensor ID, metric ID", "Which device and which quantity (e.g. BME280 / humidity)."],
        ["value and unit", "The converted value; the raw/ADC value too where relevant."],
        ["acquisition interval start / end", "When the measurement began and finished (monotonic time)."],
        ["last-good timestamp", "When the last valid value was obtained; kept separately from the current result."],
        ["sample sequence", "Increments per new sample, so duplicates and gaps are visible."],
        ["power epoch", "The rail epoch the sample belongs to (section 14)."],
        ["configuration revision, calibration revision", "Which configuration and calibration produced the value."],
        ["health and quality flags", "See the table below."],
    ], [0.36, 0.64]))
    s.append(P("Keep clock time optional; age and deadlines always use a monotonic timestamp, not NTP time."))
    s.append(table(["Health / quality", "Meaning and policy"], [
        ["STARTING / WARMING", "No valid result yet, heater/restart settling, or filter not filled. No numeric threshold action."],
        ["OK", "Fresh valid sample with declared calibration/units. Eligible for configured numeric rules."],
        ["STALE", "Previously good data has exceeded its age limit. Show age; inhibit dependent automatic actions."],
        ["FAULT / TIMEOUT", "Transport, CRC, range or conversion error. Preserve last-good separately."],
        ["UNPOWERED", "Supply unavailable or its epoch changed during measurement. Do not interpret voltage decay as a physical value."],
        ["UNCALIBRATED / DEPENDENCY_INVALID", "Acquisition works but physical units or compensation are not trustworthy. Publish raw data with explicit quality."],
    ], [0.3, 0.7]))
    s.append(P("An analog input alone usually cannot prove probe presence: disconnected inputs can float or sit at plausible voltages. Diagnose rail/range/noise anomalies, but do not promise every unplug event becomes FAULT without additional hardware evidence."))
    s.append(H2("Latest snapshot"))
    s.append(P("The active acquisition lease holder is the only writer to a length-one snapshot queue. Publish a consistent copy after each completed sensor update and at the frame boundary using xQueueOverwrite(). BLE, MQTT and Safety use xQueuePeek(…, 0): they never wait for a future measurement or consume one another's snapshot. "
               "A snapshot is a coherent record copy, not a simultaneous measurement of all sensors."))
    s.append(P("Mode/status has its own single-writer PowerCoordinator mailbox. Consumers combine it with the acquisition record and the current rail epoch. If an external-5 V sample belongs to an obsolete epoch, expose it as unavailable even if acquisition has not yet published the next snapshot. Never send a retained last-good value as a new observation."))
    s.append(H2("Freshness limits"))
    s.append(P("Initial stale limits: <b>BME280 1 s</b>, <b>ordinary 1 Hz readings 3 s</b> (this includes the SGP30). After every SGP30 initialization its outputs stay WARMING for 15 s, during which it returns fixed values (400 ppm CO2eq, 0 ppb TVOC) that must never be published as measurements. Limits are configuration, not hard-coded library behavior. [E13]"))
    s.append(P("TDS temperature compensation uses a valid DS18B20 sample and records its age/revision. If unavailable, provide raw voltage or an explicitly marked uncompensated estimate; do not silently substitute 25 degrees and call the result calibrated. [L9, L11]"))
    s.append(P("SGP30 humidity compensation, when enabled, uses absolute humidity derived from a fresh BME280 temperature and humidity sample. If that sample is stale, stop updating the compensation and report DEPENDENCY_INVALID rather than keep an old value silently. The baseline status (restored, learning, valid) is reported separately from the readings. [E13]"))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def sync():
    s = [H1("11. Synchronization and queue contracts", "s11")]
    s.append(short("Each kind of data has its own channel with a fixed capacity and a defined behavior when it is full. Latest values are overwritten, alarms are sticky so they can never be lost, commands are rejected with “busy” rather than dropped silently, and logs are dropped and counted rather than blocking acquisition."))
    s.append(G.dataflow())
    s.append(caption("Figure 6 – Data paths between tasks. Solid = data; dashed = control."))
    s.append(table(["Channel", "Owners / capacity", "Full or slow-consumer behavior"], [
        ["Latest snapshot", "Acquisition → peek readers; 1 record", "Overwrite old snapshot. Historical samples may be skipped by design."],
        ["Pending alarm store + notification", "Acquisition/ISR adapters → Safety; fixed per-source entries", "Sticky event bit plus first/worst observation and count. A later normal sample cannot erase an unhandled critical event."],
        ["Alarm detail queue", "Producers → Safety; 16 records", "Zero-wait send. On full, increment overflow count and preserve sticky alarm state."],
        ["Alarm journal", "Safety → independent BLE/MQTT cursors; 64 events", "Drop oldest detail with gap indication. Current latched alarm state remains available."],
        ["Command ingress", "Transport → Configuration; 8 small records", "Reject busy; do not silently drop or acknowledge success. Separate actuation request path to Safety."],
        ["Mode/lease control", "PowerCoordinator ↔ workers; one grant each", "ACKs carry transition generation. A stale ACK never permits another bus owner."],
        ["Telemetry backlog", "MQTT owner; 60 one-second summaries", "RAM only, drop oldest, expose count and original timestamps. No unlimited offline history."],
        ["Serial log", "All tasks → Logger; bounded, e.g. 32 entries", "Drop low-priority logs and count loss; never block acquisition."],
    ], [0.22, 0.3, 0.48]))
    s.append(H2("Critical invariants"))
    s.append(bullets([
        "No two acquisition workers hold the lease. Safety alone writes relay outputs. WiFiMqtt alone calls its MQTT client. Configuration alone commits NVS and registry updates. BLE callbacks copy validated bounded input and return; they do not touch sensor objects.",
        "Use FreeRTOS queue copying for plain fixed-size messages. A 4096-byte BLE frame uses a fixed buffer pool with explicit allocate/submit/release ownership; no stack pointers, borrowed String buffers or JsonObject views cross task boundaries. Commands carry request ID, authenticated connection/session generation and expiry.",
        "Use a short portMUX-protected rail record and sticky-alarm metadata where ISR/task coordination requires it. No I/O, blocking, allocation or large JSON copies occur inside a critical section. <i>volatile</i> is not cross-core synchronization. Hardware driver mutexes stay inside the single-owner boundary; an application mutex is not held across network calls.",
        "Separate local alarm handling from delivery. One queue read by Safety and Net would distribute events between consumers, not broadcast them. Neither a 16-entry queue nor MQTT can guarantee lossless delivery forever. The invariant is that local safety state survives detail-queue saturation, while telemetry reports any lost history.",
    ]))
    s.append(note("Why this removes the “BLE waits for a sensor” problem",
        "BLE never asks a sensor for a value. It peeks the latest snapshot (zero wait), sends it with its age and quality, and if the user wants a fresher value it submits an asynchronous refresh request. A slow or failed sensor therefore shows up as an older age or a FAULT flag – never as a blocked BLE task or a cross-core deadlock."))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def safety():
    s = [H1("12. Safety policy and control arbitration", "s12")]
    s.append(short("Safety is a small state machine on core 0 that owns the relay. It handles critical events before anything else, turns stale or missing data into alarms of their own, and never lets a remote command override a local interlock. Automatic actuation stays disabled until the relay wiring and loads are verified."))
    s.append(P("Safety is a small state machine, not a second sensor reader. Acquisition evaluates a pure threshold function against each eligible sample and submits an event immediately. Safety maintains latched alarms, stale/missing-data alarms, control leases and actuator maximum-on timers. It checks pending critical events before ordinary commands on every wake."))
    s.append(H2("Processing order on every wake"))
    s.append(bullets([
        "Inspect power validity and transition generation; invalidate unavailable inputs and reject commands for unpowered outputs.",
        "Drain sticky critical events and process pending alarm records. Take the configured local action immediately; network reporting is secondary.",
        "Evaluate freshness/dependency loss even if no new sample arrived. An acquisition stall must not keep a pump enabled using old data.",
        "Apply bounded authorized commands that pass local interlocks and expiry checks. Return accepted/rejected/applied status through the requesting transport.",
        "Publish actuator state, alarm state and diagnostics; block until another event or a bounded health-check deadline (100 ms).",
    ], num=True))
    s.append(H2("Rule definition"))
    s.append(P("Each rule contains: source metric, valid range/quality requirements, trip threshold, clear threshold, debounce or persistence duration, severity, latch policy, target actuator action and explicit reset conditions. Hysteresis prevents chatter. "
               "A severe limit may trip on one valid sample; do not add a long averaging/debounce window that contradicts its response target."))
    s.append(code("Example rule (illustrative values only - real limits are not yet commissioned)\n"
                  "  source      : BME280.temperature, quality must be OK\n"
                  "  trip / clear: 40.0 C / 37.0 C            (hysteresis 3 C)\n"
                  "  persistence : 1 valid sample              (severe limit)\n"
                  "  severity    : CRITICAL, latched\n"
                  "  action      : configured relay state, max-on timer applies\n"
                  "  reset       : valid sample below clear threshold + configured reset policy"))
    s.append(P("Faults and environmental alarms are distinct. An unpowered pH module may output zero volts; that is UNPOWERED, not an extremely acidic reservoir. Removing power must not erase an already latched real alarm. Clearing requires a valid recovery observation and the configured reset policy."))
    s.append(H2("Actuator policy is a commissioning gate"))
    s.append(P("The drawing shows one SRD-05VDC relay whose coil is fed from external 5 V and switched low-side by an IRLZ44N on GPIO 15: active-high, with a 10 kΩ gate pull-down and a 1N5822 flyback diode. It is therefore never available in low power. "
               "Still to establish: which contact (normally open or closed) is used, the driven load and its power source. The inherited code drives its relay active-low; do not port that polarity."))
    s.append(P("Default automatic actuation stays disabled until this mapping is verified. Electrical de-energization is the provisional boot/reset state; whether that is safe for circulation, aeration or valves must be decided per load. If circulation must continue on battery, its electrical branch and control requirement must be resolved together. Firmware cannot restore an unpowered actuator."))
    s.append(P("Remote control never overrides an active local interlock. Use maximum-on time and command expiry; do not replay stale actuator commands from an offline telemetry queue. [L2, L5, L7]"))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def power():
    s = [H1("13. Power-mode state machine", "s13")]
    s.append(short("A dedicated “external power present” input drives the mode. Losing it immediately invalidates the external-5 V sensors, then the system moves through ENTERING LP to LOW POWER once every task has acknowledged. Coming back requires 500 ms of stable power and per-sensor warm-up. If any task fails to acknowledge, the system reports DEGRADED instead of guessing."))
    s.append(G.power_sm())
    s.append(caption("Figure 7 – Power modes. Any loss during RESTORING aborts restoration; a failed acknowledgement leads to DEGRADED."))
    s.append(H2("Detection contract"))
    s.append(P("The external-power-present signal is GPIO 8: a 220 Ω over 2 × 220 Ω divider from the external 5 V feed, not from ESP32 VDD or the battery-backed source bus. At 5.00 V it gives ≈ 3.33 V. "
               "At nominal 3.3 V, V<sub>IH</sub> minimum is 0.75 × VDD (2.475 V) and V<sub>IL</sub> maximum is 0.25 × VDD (0.825 V). Intermediate levels, for example from a sagging feed, are not a third reliable mode. "
               "The divider has no hysteresis, so debounce the mode in firmware. GPIO 8 is also ADC1_CH7, so the actual levels and decay can be measured during commissioning. [L6, L14]"))
    s.append(P("The drawings have no rail-good signal per branch. External-source presence is not proof that the filtered 5 V branch feeding pH and TDS has settled. Use verified settle timing and report that limitation. "
               "Until the sense behavior has been measured on the board, keep the power-transition feature disabled and the build status non-production."))
    s.append(H2("Entering low power"))
    s.append(bullets([
        "On an edge, invalidate the external-5 V epoch immediately (in the ISR).",
        "PowerCoordinator starts a proposed 50 ms debounce, asks acquisition to quiesce at its next bounded operation boundary, requests WiFiMqtt shutdown and asks Safety to apply unavailable-actuator policy. These operations do not wait for MQTT delivery. Logger drains a bounded amount and blocks.",
        "After the active worker cancels/finishes operations, sets powered-off interface pins safe and releases the lease, PowerCoordinator grants the low-power worker the new generation.",
        "BLE announces LOW_POWER only when mandatory transition acknowledgements are complete; otherwise report TRANSITION/DEGRADED. Low-power acquisition keeps the four 3.3 V sensors running at their declared periods.",
    ], num=True))
    s.append(H2("Returning to normal"))
    s.append(bullets([
        "Require stable external power (initial proposal: 500 ms) and verified rail settling.",
        "Quiesce the low-power worker, invalidate pending conversions/filter windows as needed, then grant the normal worker the new generation.",
        "Reinitialize restored devices and mark each WARMING until individually valid.",
        "Request Wi-Fi reconnect asynchronously. Normal sensing must not wait for a broker.",
    ], num=True))
    s.append(H2("Boot and failure rules"))
    s.append(P("Boot with absent/unknown power takes the conservative low-power policy; invalid sense configuration prevents a production-ready status. A new falling edge during recovery aborts recovery. "
               "Transition ACKs have a proposed <b>200 ms</b> deadline, except network shutdown's separately bounded <b>2 s</b> deadline. Missing ACKs enter DEGRADED; they never justify granting two owners or falsely reporting complete shutdown."))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def rails():
    s = [H1("14. Rail races and safe ownership handover", "s14")]
    s.append(short("Removing external power can cut the 5 V sensors at any instant, even in the middle of a reading. Every 5 V measurement therefore records the rail epoch when it starts and is accepted only if the epoch is unchanged when it ends. Moving the sensors between the two acquisition workers follows a strict quiesce → release → grant protocol so two tasks never drive the same bus."))
    s.append(H2("Why a boolean is insufficient"))
    s.append(P("Checking rail5vValid before and after a read misses a loss-and-return sequence if the flag becomes true again between checks. Use a monotonically changing rail epoch with the validity flag. The ISR increments the epoch and clears validity on every observed sense edge; only PowerCoordinator can restore validity after stability/rail checks. "
               "Debounce the mode, not the initial invalidation."))
    s.append(code("Before 5 V operation: copy {valid, epoch}; refuse if !valid.\n"
                  "Start bounded measurement; tag pending conversion with epoch.\n"
                  "On completion: copy rail state again.\n"
                  "Accept only if still valid AND epoch is unchanged.\n"
                  "Check epoch again when Safety/transport consumes the result.\n"
                  "Otherwise: discard numeric result; publish UNPOWERED/TRANSITION."))
    s.append(G.epoch_timeline())
    s.append(caption("Figure 8 – Power drops and returns during one pH filter window. A boolean-only check can accept the reading; the epoch check rejects it."))
    s.append(example("USB unplugged during a pH read",
        "1. Rail state is {valid, epoch 7}. AcquisitionNormal copies it and starts the pH filter window.<br/>"
        "2. The USB cable is pulled. The external 5 V feed and its filtered branch collapse, so the pH module output decays toward 0 V (the inherited pH formula would read about pH −8.8 here). The ESP32-S3 keeps running from the battery path through the source bus.<br/>"
        "3. The power ISR runs on core 0: validity = false, epoch = 8, notify PowerCoordinator. Nothing else happens in the ISR.<br/>"
        "4. The cable reconnects a few milliseconds later: the ISR runs again, epoch = 9, validity stays false. Only PowerCoordinator may restore it, after 500 ms of stable power.<br/>"
        "5. The pH window ends. The worker re-copies the rail state: epoch 9 ≠ 7, so the numeric result is discarded and pH is published as UNPOWERED. No pH alarm is raised; any alarm latched before the cut stays latched."))
    s.append(P("Short critical sections make rail-state copies coherent across cores. Never keep the rail lock while converting or reading a bus. Keep ISR operations audited and ISR-safe; a std::atomic declaration alone is not proof of interrupt/cache safety. "
               "Queue messages and peripheral completion events include the transition generation so delayed callbacks cannot resurrect an old result."))
    s.append(note("Limit of the guarantee",
        "This protects against <i>observed</i> transitions. If physical voltage collapses before the sense interrupt is serviced, software cannot prove that a sample taken in that interval is valid. Scope the power-sense/rail relationship and interrupt delay; use rail-good hardware for a tighter guarantee. Firmware is not a substitute for adequate brownout and supply design."))
    s.append(H2("Lease protocol"))
    s.append(G.lease_seq())
    s.append(caption("Figure 9 – Handing the sensor buses from one acquisition worker to the other."))
    s.append(bullets([
        "PowerCoordinator sends QUIESCE(generation) to the current worker. The worker stops launching operations and wakes from bounded waits.",
        "Worker cancels capture, completes or discards transactions, disables unsafe output driving, unregisters its active watchdog obligation and relinquishes context access.",
        "Worker sends RELEASED(generation) and blocks awaiting a future grant. It must not touch the context after RELEASED.",
        "Coordinator verifies generation and ownership, then issues GRANT(new generation, allowed rail mask) to the other worker. No ACK means no grant.",
        "New worker validates pending state, restores only allowed interfaces and begins scheduling. ISR/capture handlers deliver to fixed queues or generation-tagged handles, never stale task pointers.",
    ], num=True))
    s.append(P("For HC-SR04, hold TRIG low during shutdown, then use the verified non-backfeeding pin configuration. Disable pull-ups or active driving into any unpowered board. Drive GPIO 15 low before release; the gate pull-down then keeps the relay driver off. "
               "Peripherals/interrupts with core-local allocation rules must be installed and released from their required core or use a fixed interrupt broker. Validate the chosen HAL before enabling the handover feature. [E2, L5–L6]"))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def lowpower():
    s = [H1("15. Low-power scope and framework choice", "s15")]
    s.append(short("The first low-power mode switches off Wi-Fi/MQTT and the external-5 V sensors, keeps BLE and the four 3.3 V sensors, and runs everything on core 0. Core 1 is idle, not powered off: an ESP32-S3 cannot turn a core off at runtime. Automatic light sleep is not available with the chosen Arduino build; measure the real current before promising battery life."))
    s.append(H2("Baseline build"))
    s.append(P("Use the demo's pinned PlatformIO espressif32@6.12.0 platform, which selects the Arduino 2.0.17 package family, with the esp32-s3-devkitc-1 board definition configured for the N16R8 (QIO flash, OPI PSRAM, 16 MB partition table). Keep NimBLE-Arduino 2.5.1 and ArduinoJson 6.21.5 pinned to preserve the demo protocol baseline; lock additional dependencies only after a compile and API check. "
               "This is a reproducibility choice, not a claim that these are the latest releases. [E5, L3]"))
    s.append(P("The Stage 1 build resolved framework-arduinoespressif32 3.20017.241212. The upstream Arduino 2.0.17 ESP32-S3 sdkconfig has CONFIG_PM_ENABLE disabled, a 1000 Hz tick, dual-core FreeRTOS, Bluetooth controller affinity on core 0, loop() on core 1 and a 5 s task watchdog. "
               "Confirm the same values in the resolved package before relying on them. [E6]"))
    s.append(H2("What the first low-power mode can deliver"))
    s.append(table(["Item", "Normal mode", "Low-power mode"], [
        ["Wi-Fi / MQTT", "On", "Off (Wi-Fi disconnect and WIFI_OFF, performed by the WiFiMqtt service)"],
        ["BLE", "On", "On"],
        ["Sensors", "All qualified sensors", "BME280, SGP30, DS18B20, DFR0026 only (retained 3.3 V supply)"],
        ["Acquisition", "AcquisitionNormal, core 1", "AcquisitionLowPower, core 0"],
        ["Serial logging", "On (core 1)", "Blocked; diagnostics via BLE"],
        ["CPU frequency", "160 MHz (proposed)", "80 MHz is an optimization candidate: it must pass BLE, bus timing and deadline tests; switch only at a quiescent boundary and check the API result"],
        ["Core 1", "Application work", "Application tasks blocked; idle (WAITI). Scheduler interrupts and SDK work may still execute"],
    ], [0.18, 0.27, 0.55]))
    s.append(P("There is no supported runtime application switch from a dual-core FreeRTOS image to a unicore image. CONFIG_FREERTOS_UNICORE is a build option. Core 1 can be mostly idle with its application tasks blocked, but scheduler interrupts and SDK work may still execute. "
               "“Single core” must be reported as application affinity, with measured current savings, not as power-gating proof. [E2]"))
    s.append(H2("Deeper sleep is a later decision"))
    s.append(P("Automatic DFS/light sleep requires a compatible SDK built with power management and tickless idle. Defining a compiler macro in application code does not rebuild Arduino's precompiled SDK. BLE light sleep also depends on the Bluetooth low-power clock source; check the ESP32-S3 options in the IDF guide before assuming an external 32 kHz crystal, which the current drawing does not show. [E3, E6]"))
    s.append(P("If measurements show the baseline cannot meet battery targets, evaluate ESP-IDF or Arduino as an ESP-IDF component with a controlled sdkconfig. Keep driver/domain interfaces portable for that route. Do not promise connected BLE during manual deep sleep; deep sleep is incompatible with the requested continuously available link. [E9]"))
    s.append(P("Measure whole-board current in normal, Wi-Fi-off dual-core, single-core application, and reduced-frequency modes. Regulator losses, relay coils, sensor LEDs and heater loads matter. The SGP30 heats its sensing element continuously and stays on the battery branch, so measure its share explicitly. This plan makes no battery-runtime claim."))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s
