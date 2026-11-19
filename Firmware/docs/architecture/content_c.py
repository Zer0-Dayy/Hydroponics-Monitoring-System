# Sections 16-22.
from kit import *
from reportlab.platypus import PageBreak, CondPageBreak


def sensors():
    s = [H1("16. Sensor contracts and bring-up decisions", "s16")]
    s.append(short("Every driver has the same six-function interface and never hides a long delay. Each sensor is “done” only when it has passed its own evidence list, not when it compiles. The SGP30 needs particular care: it must be measured every second without gaps, and its first outputs and its baseline need explicit states."))
    s.append(P("Each driver exposes:"))
    s.append(table(["Function", "Contract"], [
        ["init(config)", "Probe and configure the device; report identity/status."],
        ["start(now)", "Begin a measurement or conversion without waiting for it."],
        ["poll(now)", "Return PENDING / READY / error and a next-ready deadline. No hidden long delay."],
        ["readResult()", "Return the typed, bounded result of the finished measurement."],
        ["cancel()", "Abandon an in-progress operation (used by quiesce)."],
        ["preparePowerOff()", "Put pins and device into the safe, non-backfeeding state before a rail is cut."],
    ], [0.22, 0.78]))
    s.append(P("A driver can wrap a proven manufacturer library, but its worst blocking operation must be documented and bounded."))
    s.append(table(["Driver", "Initial implementation contract", "Completion evidence"], [
        ["BME280", "Verify chip ID; forced mode, oversampling ×1 T/P/H; read compensation coefficients; use Bosch compensation; 12 ms planned completion budget.", "Known reference comparison; missing device, stuck bus and conversion timeout; timestamps and 250 ms schedule."],
        ["DS18B20", "External 3.3 V supply; select stable ROM ID; 12-bit asynchronous conversion, 750 ms limit; scratchpad CRC.", "Missing/CRC error handling; first-conversion state; no blind acceptance of the 85 °C power-on value. A real 85 °C result is valid after confirmed conversion."],
        ["DFR0026", "Read calibrated ADC millivolts/raw; characterize filter and saturation.", "Dark/bright response and unplug behavior characterized; lux only after reference-meter calibration."],
        ["HC-SR04", "10 µs trigger; bounded capture of ECHO; 30 ms timeout; measured mounting geometry.", "No echo, stuck high/low and invalid distance; empty/full tank mapping checked physically."],
        ["pH-4502C", "ADC calibration → divider correction → measured pH fit; timestamp spaced filter window.", "At least two appropriate buffer points, independent check buffer and settling/noise measurements. Exact board documentation remains weak."],
        ["SEN0244 TDS", "Preserve raw voltage; valid sample-count filter; calibrated transfer with fresh water temperature.", "Reference solution and temperature tests; invalid dependency and saturation handling. Do not equate TDS estimate with calibrated EC."],
        ["SGP30", "Initialize after every power-up or reset; one measurement per second from the absolute frame; TVOC and CO2eq with a CRC check on every word; restore a stored baseline; optional absolute-humidity compensation from the BME280.", "Fixed initial values marked WARMING; CRC error and missing-device handling; baseline save/restore across a reset; clean-air and known-VOC-source checks; CO2eq labelled as an estimate."],
    ], [0.14, 0.43, 0.43], hl3_rows=(6,)))
    s.append(H2("The SGP30 needs a steady cadence and a baseline"))
    s.append(P("The SGP30's on-chip compensation assumes a measurement command about once per second. For the first 15 s after initialization it returns fixed values, and its baseline must either be restored from storage or learned over a long run before values are fully compensated [E13]. "
               "Schedule it from the same absolute frame in both power modes (it stays on the retained supply), count missed seconds, store the baseline through Configuration at a rate-limited interval, and report baseline status beside the readings."))
    s.append(P("Report TVOC in ppb and CO2eq in ppm as an estimate, never as a CO2 measurement. The SGP30 chip itself is a 1.8 V part; confirm that the fitted module regulates its supply and shifts the I2C levels from 3.3 V. [E13, L7]"))
    s.append(new3("MQ-7 removed",
        "Rev 1–2 planned an MQ-7 CO sensor with a heater-phase state machine. It is no longer part of the hardware (Redesign Specification Rev 5). In the firmware, SENSOR_MQ7 and METRIC_CO_SIGNAL were replaced by SENSOR_SGP30, METRIC_TVOC and METRIC_CO2EQ (Snapshot 528 → 584 bytes), and drivers/mq7.* by the drivers/sgp30.* stub with its test_sgp30 environment. L10 is kept only for traceability."))
    s.append(P("Analog disconnected-probe detection and pH module thresholds require board-specific evidence. A successful compile or plausible serial number does not establish calibrated accuracy. [L8–L13]"))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def comms():
    s = [H1("17. BLE, MQTT and configuration boundary", "s17")]
    s.append(short("Keep the existing BLE protocol (UUIDs, framing, secure pairing) but make it carry real health and power information. Wi-Fi/MQTT retries with backoff and never restarts the board. Configuration changes are validated, saved as a versioned whole and applied only at a safe moment."))
    s.append(H2("Preserve the useful protocol"))
    s.append(P("Keep the existing service/characteristic UUID map and two-byte little-endian length framing, with a 4096-byte maximum JSON payload. Retain authenticated access, LE Secure Connections/bonding and request IDs. Implement physical commissioning and owner-recovery policies; the demo currently has no time-limited commissioning window. Do not claim the demo's ownership scheme alone resolves all production authorization needs. [L4]"))
    s.append(P("State continues every 2 s and on a committed mode change; telemetry may remain every 3 s while subscribed, as in the app contract. Both consume available records immediately. Include sample age, health, power availability, sequence and configuration revision. No valid numeric value is sent for UNPOWERED. Omit battery percentage until it is actually measured and estimated."))
    s.append(P("The demo advertises pH/TDS/level as low-power essentials and pauses climate/light: reverse this policy to match the physical rails. Its climate telemetry carries only one metric; plan support for BME280 humidity/pressure, SGP30 TVOC/CO2eq and baseline status, water temperature, unavailable readings and schema negotiation in Protocol.py, Catalog.py and the UI. Treat this as a coordinated protocol revision, not a promise of unchanged-app compatibility."))
    s.append(H2("Bounded transport services"))
    s.append(P("<b>BLE.</b> Callbacks validate length/session, copy to a fixed pool and enqueue without blocking. BleTransport serializes complete frames per connection, prioritizes results/alarms over periodic telemetry and stops sending after disconnect/session change. Partial frame failure resets framing through an explicit reconnect/resync policy. A notification does not prove the laptop processed an alarm; use event IDs and acknowledgement if that guarantee is required."))
    s.append(P("<b>Wi-Fi/MQTT.</b> WiFiMqtt owns its connection state machine. Use finite socket/connect timeouts and exponential retry backoff, initially 1, 2, 4, 8 seconds up to 60 seconds with jitter. No broker failure restarts the sensing system. Power-stop requests must preempt retry waiting and have a bounded shutdown path. Verify client calls actually yield and honor deadlines; move to an asynchronous MQTT client if they cannot."))
    s.append(H2("Configuration transactions"))
    s.append(P("Retain get_devices / set_wifi / add_device / delete_device through validated commands. Board pin mappings remain fixed initially; runtime logical instances may only use supported predeclared slots. Request quiescence before changing a driver's context; never delete an object from a BLE callback. "
               "Validate, stage, persist a versioned configuration, apply it at a safe boundary, then acknowledge the actual outcome. Recovery loads the last valid committed version."))
    s.append(P("NVS writes and flash erase can affect both cores. Rate-limit writes and schedule a controlled maintenance window with reported timing status; test its worst-case pause. Credentials are provisioned and never logged. Production remote actuation requires authenticated broker access and authorization; preserve legacy MQTT topic shapes only through an adapter."))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def faults():
    s = [H1("18. Fault tolerance, deadlines and recovery", "s18")]
    s.append(short("Every failure has a defined behavior: mark the data, keep sensing and local safety going, report it, retry later within bounds, and reset only as a last resort with the reason recorded. Watchdogs prove that each task is making progress, not merely that the CPU is alive."))
    s.append(table(["Failure", "Required behavior"], [
        ["I2C missing device / stuck bus", "Bounded transaction timeout; mark affected readings FAULT; schedule limited bus recovery under sole ownership. No repeated blocking retry loop. A stuck bus affects both the BME280 and the SGP30."],
        ["SGP30 CRC error / missed second", "Reject the word, count the gap; after repeated gaps re-initialize and return to WARMING."],
        ["DS18B20 CRC / no presence", "Reject result, retain last-good timestamp separately, retry at a bounded later deadline."],
        ["Analog saturation / unknown calibration", "Expose raw value and quality; suppress invalid derived-unit alarm decisions."],
        ["Power edge during conversion", "Discard by epoch; safe interface configuration; no false numeric alarm."],
        ["Broker or access point absent", "Continue sampling and local safety; BLE reports network state; bounded backoff/backlog."],
        ["BLE congestion / disconnect", "Drop superseded telemetry; preserve current alarm state; reject expired session commands."],
        ["Alarm / command queue full", "Sticky local alarm persists; count lost detail; command sender receives busy/error rather than false success."],
        ["Acquisition deadline miss", "Record lateness and sensor duration; skip expired work rather than burst-catching up; set degraded timing status."],
        ["Transition ACK missing", "No second owner; keep unavailable rails invalid; local safe policy; supervisor diagnoses and escalates."],
        ["Task stalled / heap exhausted", "Safe outputs where possible, persistent reset reason at next boot, bounded restart strategy; no endless restart loop."],
    ], [0.3, 0.7]))
    s.append(H2("Watchdog design"))
    s.append(P("Use SDK task/interrupt watchdogs according to the pinned version. Each monitored task resets its own watchdog after completing meaningful work; Supervisor must not blindly feed it on behalf of stalled tasks. Idle subscription catches CPU starvation; a separate monotonic progress timestamp catches a task blocked forever while idle still runs. [E10]"))
    s.append(P("Inactive acquisition workers deliberately block forever and are excluded from active-work progress deadlines. Update expected-task membership as part of the acknowledged lease transition. Safety's timed wait and the network state machine report progress even when there is no sensor event or broker connection. "
               "Start with a five-second task-watchdog budget only if every underlying call is proven shorter; a timeout counter and recovery attempt must precede reset where possible."))
    s.append(H2("Bounded memory and logs"))
    s.append(P("Allocate queues, pending-alarm entries, frame pools and sensor contexts once. Check every creation result. No dynamic JSON construction in acquisition or ISRs. Measure minimum free heap, largest free block and stack high-water marks during BLE/MQTT overlap. The initial stack table is not a memory-fit proof."))
    s.append(P("Rate-limit serial printing through Logger, with a short write timeout. In low power the logger acknowledges quiescence and blocks; diagnostics remain available through BLE. Backlog detail may be lost on power/reset, but current alarm and fault status must be reconstructible from local state. Do not write every sample to flash."))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def stages():
    s = [H1("19. Implementation stages and acceptance", "s19")]
    s.append(short("Build sensor-first: the shared data model and one driver at a time, each with its own test environment. Then the scheduler and Safety with fake data, then BLE/MQTT, and only last the power-switching machinery. Each stage has measurable acceptance tests."))
    s.append(H2("Sensor-first development sequence"))
    s.append(bullets([
        "Create firmware/ with the board profile, common Reading model and a small RTOS bring-up harness. Pin dependencies and inspect the resolved SDK configuration; compile for the actual ESP32-S3 board.",
        "Implement BME280, then DS18B20, then DFR0026, then SGP30 (it reuses the BME280's I2C HAL). Each test_&lt;sensor&gt; environment links the production driver and one bring-up main; exclude production main so entry points cannot collide.",
        "Implement HC-SR04, pH and TDS after each device's electrical prerequisites are satisfied. Calibration is a separate acceptance milestone from communication.",
        "Integrate the acquisition scheduler, Safety and copied snapshots. Inject fake time/data to test ordering, stale values and alarm latching before network integration.",
        "Add MQTT/BLE adapters and bounded commands. Update and test the desktop protocol for physical health and power policy.",
        "Add power ISR, rail epochs, lease handover and measured power optimizations. Do not activate transitions until the hardware sense input and rail behavior are verified.",
    ], num=True))
    s.append(new("D15: one acquisition worker until stage 6",
        "Stages 1–5 run a single acquisition task on core 1 that owns all buses directly. The second (low-power) worker, the lease/generation messages and the QUIESCE/RELEASED/GRANT handover of section 14 are added together in stage 6, when the power-sense input exists to drive them. "
        "The driver interface (start/poll/cancel/preparePowerOff) and the generation field in records are present from stage 1, so stage 6 adds the protocol without changing drivers. This keeps early sensor work simple without creating rework."))
    s.append(table(["Test", "Acceptance target / required evidence"], [
        ["Per-driver build", "pio run -e test_&lt;sensor&gt; succeeds; recorded dependency versions and memory use. Build records are kept in firmware/README.md."],
        ["Sensor bench checks", "Timestamped readings, reference comparison, startup and disconnect behavior; record what analog disconnection cannot detect."],
        ["One-second schedule", "Logic-analyzer/monotonic trace: 1 s frame release, proposed ≤ 5 ms release jitter under specified healthy radio load. Record max and percentiles."],
        ["Temperature alarm", "Inject a sustained threshold crossing at varied schedule phases: ≤ 300 ms software detection/action target and ≤ 10 ms from accepted sample to Safety action. Physical thermal response measured separately."],
        ["Safety under radio load (new)", "Repeat the temperature-alarm test while Wi-Fi/MQTT publishes continuously and BLE streams telemetry. Record the sample-to-action distribution and the SDK task priorities seen in the trace. If ≤ 10 ms is not met, apply the D14 fallback and retest."],
        ["Power transitions", "Drop/reapply power at every driver step, plus rapid bounce and loss-return within a read. No stale epoch accepted; no false pH/TDS numeric alarm; only one worker owns buses."],
        ["Network faults", "AP/broker absent for 30 min; repeated BLE connect/disconnect and frame congestion; sensing continues, storage bounded and drop counters accurate."],
        ["Queue overload", "Fill command/alarm queues; verify busy results, sticky alarms, independent transport delivery and event-gap reporting."],
        ["Stall injection", "Halt each active worker deliberately; progress watchdog detects it. Expected idle workers do not cause false resets."],
        ["Power/current", "Compare modes at the supply input with identical workload; verify Wi-Fi off, BLE availability, readings from all four 3.3 V sensors and core-1 application blocking. Measure the SGP30's share of the battery current."],
        ["Endurance", "24 h mixed-mode run with counters, max latency, heap/stack trends and reset causes; no unexpected watchdog reset or unbounded growth."],
    ], [0.24, 0.76], highlight_rows=(4,)))
    s.append(Spacer(1, 6))
    s.append(P("<b>Commissioning gates:</b> measured power-sense levels on GPIO 8, branch settle timing, relay contact/load policy, SGP30 module checks, analog ranges/calibration, and final trip thresholds. These gates do not prevent isolated BME280 driver work from starting next."))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def decisions():
    s = [H1("20. Decision log and remaining gates", "s20")]
    s.append(short("Eighteen recorded decisions, each with the alternative it rejects (D02 is kept but superseded by D16). Changing one of them means updating this table and the sections that cite it. The open hardware items below must be closed before the matching features are switched on."))
    s.append(table(["ID", "Decision", "Rationale / alternative rejected"], [
        ["D01", "New firmware directory; preserve old code", "Selective porting gives a clean boundary without discarding useful protocol knowledge."],
        ["D02", "ESP32-WROOM-32D remains target (superseded by D16)", "Rev 1–2 decision, kept for traceability. The board changed to the ESP32-S3 in October 2026."],
        ["D03", "Explicit Espressif FreeRTOS", "Supported kernel and radio integration; no second RTOS or bare-metal replacement."],
        ["D04", "One driver per sensor, thin main", "Keeps sensor bring-up familiar; services own orchestration instead of a growing main."],
        ["D05", "One serial acquisition owner", "Removes peripheral races without creating one task and mutex set per sensor."],
        ["D06", "1 s frame + 250 ms BME280 checks", "Predictable baseline and shorter software blind interval; does not claim a hardware temperature interrupt."],
        ["D07", "Core 1 acquisition, core 0 networking/Safety", "Matches typical ESP32 radio allocation and avoids migrating safety state. Explicit pinning intentionally differs from Rev 4's conditional affinity guidance."],
        ["D08", "Pre-created workers with generation lease", "Avoids task-delete/recreate races and repeated allocation; costs one extra stack. Handover still requires peripheral-affinity validation."],
        ["D09", "Latest snapshots, sticky safety state", "Fast readers never wait for sensors; finite event queues no longer imply impossible lossless delivery."],
        ["D10", "Power epoch, not only a boolean", "Detects power loss and return during a pending measurement."],
        ["D11", "Awake low-power baseline", "Preserves BLE; automatic sleep requires a separate SDK/hardware decision."],
        ["D12", "Rail-based availability, not demo essential flag", "pH/TDS/level cannot operate when their external 5 V supply is removed."],
        ["D13", "Calibrated units only after evidence", "pH fit, TDS compensation, light calibration and the SGP30 baseline/humidity compensation are explicit quality gates."],
        ["D14", "Safety stays on core 0, verified under radio load", "SDK radio tasks outrank Safety on core 0. Keep D07 but make the ≤ 10 ms target a radio-load test; fallback is Safety on core 1 in normal mode with a quiesce/grant handover. Rejected: assuming application priority 6 bounds latency."],
        ["D15", "Single acquisition worker until stage 6", "Early sensor work stays simple; interfaces already carry generation fields so the lease is added without driver rework. Rejected: building the full handover before the power-sense hardware exists."],
        ["D16", "ESP32-S3-WROOM-1-N16R8 is the target (supersedes D02)", "Hardware changed. Use only the GPIOs on the 22 October drawing; keep stacks, queues and ISR data in internal SRAM and use PSRAM only for measured bulk buffers. Rejected: keeping a WROOM-32D pin map."],
        ["D17", "Project code in C on the Arduino-ESP32 framework", "Migrated 16 October 2026 with layer rules, contracts and record sizes unchanged. C++ libraries only behind an adapter. Rejected: moving to bare ESP-IDF now (section 15 keeps that route open)."],
        ["D18", "SGP30 on the retained 3.3 V supply replaces the MQ-7", "Per Redesign Specification Rev 5. It shares the I2C bus, needs no heater-profile hardware or 5 V divider and stays available on battery. It reports TVOC and an estimated CO2eq, not CO. Rejected: keeping the MQ-7, whose specified heater cycle needed hardware the module did not expose."],
    ], [0.07, 0.33, 0.6], highlight_rows=(13, 14), hl3_rows=(1, 11, 12, 15, 16, 17)))
    s.append(H2("Open hardware and application decisions"))
    s.append(table(["Open item", "Blocks"], [
        ["External-power sense on GPIO 8: measured levels, decay and bounce", "Power ISR and low-power transitions (stage 6)"],
        ["Per-branch rail feedback (not on the drawing)", "Tighter rail-validity guarantee (section 14)"],
        ["Measured outage/return behavior, including the source-selection transfer", "Confirming the 50 ms debounce and 500 ms restore values"],
        ["Relay contact use (NO/NC), driven load and load-safe state", "Any automatic or remote actuation"],
        ["SGP30 module: supply and logic levels, pull-ups, baseline storage policy", "Trustworthy TVOC/CO2eq; battery current"],
        ["pH / light ADC headroom against the S3's ~3.1 V range", "Accuracy at the top of their ranges"],
        ["All genuine trip/clear limits", "Enabling Safety actions"],
    ], [0.6, 0.4], hl3_rows=(0, 3, 4, 5)))
    s.append(Spacer(1, 6))
    s.append(P("The schematic now contains one relay, while the BLE catalog can describe four. Plan the initial board profile for only physically confirmed outputs. Adding more logical device entries cannot create more pins or supplies. Battery-backed circulation, if required, needs an explicit hardware and firmware requirement."))
    s.append(H2("What is complete in this phase"))
    s.append(P("Revisions 1–2 reviewed the source and drawings, checked manufacturer/SDK constraints and produced the architecture and staged validation plan. Since then the Stage 1 skeleton has been built in C for the ESP32-S3, the BME280 runs on S3 hardware and the pH driver is written (firmware/README.md). "
               "Revision 3 brings this document in line with the 22 October hardware. No calibration, power-transition behavior or power saving is claimed as validated. Circuit documents were not edited."))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def evidence():
    s = [H1("21. Local evidence register", "s21")]
    s.append(short("The files on which this document's claims rest. Paths are relative to the workspace root; line numbers refer to the files as inspected on 4 October 2026. L5–L8 and L14 were updated on 22 October 2026."))
    s.append(table(["Ref", "Source and relevant location"], [
        ["L1", "User request and attached Pasted text.txt, attachment 7665e9d2-49da-4fbc-a419-b8acb71c80a6. Treated as a proposed draft; its unsupported guarantees were corrected."],
        ["L2", "Hydroponics-Monitoring-System-master/Project Handover/Source Code/src/: main.cpp, connectToWifi.cpp, sensor.cpp, deviceManager.cpp, deviceFactory.cpp and all sensor/relay implementations inspected."],
        ["L3", "Hydroponics-Monitoring-System-master/BluetoothControlApp/ESP-Demo/src/Main.cpp and ESP-Demo/platformio.ini. setLowPower at line 202; command queue creation at line 524."],
        ["L4", "Hydroponics-Monitoring-System-master/BluetoothControlApp/FirmwareContract.md. Version 1 GATT, framing, security intent and simulation limits."],
        ["L5", "Electrical System Study/Power Circuit Diagram (1).pdf, page 1. Charger, 3S battery and BMS, MP1584 5 V / 3.3 V regulators, IRLZ44N source selection, branch decoupling, ferrite-filtered pH/TDS branch, relay and per-sensor supplies."],
        ["L6", "Electrical System Study/Pin Layout (1).pdf (Sensor ↔ ESP32-S3 Signal Pinout, 22 October 2026) and Pin Layout.drawio. Signal assignments, resistor and capacitor values, strapping/PSRAM/USB pins and unconnected comparator outputs."],
        ["L7", "Electrical System Study/Redesign Specification.pdf, Rev 5 (16 October 2026), sections 2–7. Branch availability, signal table, SGP30 requirements, power sensing and task allocation."],
        ["L8", "Electrical System Study/Datasheets/BME-280.pdf. Bosch Rev 1.23; sections 3, 4, 7.1 pin description and 9.1 measurement timing."],
        ["L9", "Same Datasheets directory: DS18B20.pdf. Maxim Rev 6; page 3 conversion time, pages 5–7 conversion/power/alarm behavior, page 9 CRC."],
        ["L10", "Same directory: MQ-7.pdf. Historical only: the MQ-7 was removed from the hardware in Rev 3 (D18)."],
        ["L11", "Same directory: SEN0244.PDF. DFRobot, pages 1–3 electrical limits, pages 4–7 filtering and temperature compensation."],
        ["L12", "Same directory: HC-SR04.pdf. Supplied module guide, pages 2–5: trigger/echo, supply and measurement limitations. Generic module documentation; verify actual board."],
        ["L13", "Same directory: DFR0026.PDF, pages 1–4, and PH-Sensor-4502C.pdf, pages 1–6. The pH file is a tutorial, not an authoritative manufacturer electrical specification."],
        ["L14", "Same directory: ESP32-S3-WROOM-1U-N16R8.pdf. Espressif WROOM-1/1U datasheet v1.0: pin functions and ADC1 channels, PSRAM use of IO35–37, strapping pins (section 3.3) and DC characteristics (table 9)."],
    ], [0.07, 0.93], hl3_rows=(4, 5, 6, 7, 9, 13)))
    s.append(H2("Evidence hierarchy"))
    s.append(bullets([
        "The current request determines intent.",
        "The current electrical diagram determines intended topology.",
        "The pin drawing determines candidate signals.",
        "Exact-part datasheets constrain implementation.",
        "Existing code proves only what the inherited software does, and a demo proves only its simulated behavior.",
    ], num=True))
    s.append(P("Where these sources disagree, this report records a gate instead of silently selecting a convenient interpretation. A baseline hash manifest and post-generation preservation check accompanied the Rev 1 document QA."))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def references():
    s = [H1("22. Manufacturer and software references", "s22")]
    s.append(short("Primary online sources checked on 4 October 2026; the ESP32-S3 links and E13 were checked on 22 October 2026. Versioned links deliberately match the Arduino baseline where possible; the local sensor PDFs in section 21 remain the detailed sensor evidence."))
    L = lambda url, t: f'<link href="{url}" color="#12807a"><u>{t}</u></link>'
    s.append(table(["Ref", "Reference", "Used for"], [
        ["E1", L("https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf", "Espressif ESP32-S3-WROOM-1/1U datasheet"), "Processor, pins, electrical limits and power modes (online copy of L14)."],
        ["E2", L("https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32s3/api-guides/freertos-smp.html", "ESP-IDF v4.4.8 FreeRTOS SMP (ESP32-S3)"), "Affinity, queues/critical sections, deletion and unicore configuration."],
        ["E3", L("https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32s3/api-reference/system/power_management.html", "ESP-IDF v4.4.8 power management (ESP32-S3)"), "PM enablement, locks, tickless idle and BLE clock conditions."],
        ["E4", L("https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32s3/api-reference/peripherals/adc.html", "ESP-IDF v4.4.8 ADC (ESP32-S3)"), "ADC1/ADC2, Wi-Fi conflict, attenuation (0–3100 mV at 11 dB) and calibration."],
        ["E5", L("https://raw.githubusercontent.com/platformio/platform-espressif32/v6.12.0/platform.json", "PlatformIO espressif32 v6.12.0 manifest"), "Framework package selection."],
        ["E6", L("https://raw.githubusercontent.com/espressif/arduino-esp32/2.0.17/tools/sdk/esp32s3/sdkconfig", "Arduino-ESP32 2.0.17 ESP32-S3 sdkconfig"), "Upstream PM, tick and Bluetooth-controller configuration."],
        ["E7", L("https://github.com/h2zero/NimBLE-Arduino/blob/2.5.1/src/nimconfig.h", "NimBLE-Arduino 2.5.1 configuration"), "Library configuration entry point; inspect actual host affinity during integration."],
        ["E8", L("https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/bt/ble.html", "Espressif Bluetooth LE FAQ"), "Bluetooth LE low-power considerations."],
        ["E9", L("https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/sleep_modes.html", "Espressif ESP32-S3 sleep modes"), "Connection-preserving modem/automatic sleep versus manual sleep."],
        ["E10", L("https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32s3/api-reference/system/wdts.html", "ESP-IDF v4.4.8 watchdogs (ESP32-S3)"), "Task/interrupt watchdog semantics."],
        ["E11", L("https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme280-ds002.pdf", "Bosch BME280 datasheet"), "Manufacturer source corresponding to the supplied local document."],
        ["E12", L("https://wiki.dfrobot.com/dfr0026/", "DFRobot DFR0026 product documentation"), "Analog ambient-light module characteristics."],
        ["E13", L("https://sensirion.com/products/catalog/SGP30", "Sensirion SGP30 product page and datasheet"), "Measurement cadence, initialization values, CRC, baseline and humidity compensation. No local copy yet: add it to the Datasheets folder."],
    ], [0.07, 0.43, 0.5], hl3_rows=(0, 1, 2, 3, 5, 8, 9, 12)))
    s.append(Spacer(1, 6))
    s.append(new("Framework version note",
        "The ESP-IDF links above are the v4.4.8 documentation, but Arduino-ESP32 2.0.17 is built on an earlier 4.4.x release of ESP-IDF (4.4.7 according to its release notes). The architecture conclusions do not depend on the patch level, but every API, Kconfig option and default used in code must be checked against the IDF version actually resolved by the PlatformIO build (section 19, stage 1)."))
    s.append(H2("How to use this document during implementation"))
    s.append(P("Keep the requirement IDs, decision IDs and acceptance tests visible during each sensor bring-up. Replace proposed timing budgets with measured maximums, record the exact library/build configuration, and update only the decisions affected by evidence. "
               "Preserve the distinction between reading a sensor, obtaining a calibrated value, and demonstrating an end-to-end safety response."))
    s.append(P("The first coding deliverable should be the shared minimal data model plus the BME280 driver and its isolated RTOS bring-up environment. It does not depend on choosing whether every old source file will eventually be retired."))
    return s
