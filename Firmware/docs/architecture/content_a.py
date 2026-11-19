# Front matter and sections 1-8.
from kit import *
import diagrams as G
from reportlab.platypus import PageBreak, CondPageBreak, NextPageTemplate


def front():
    s = [NextPageTemplate("body")]
    s.append(Spacer(1, 50 * mm))
    s.append(Paragraph("Firmware Architecture Baseline", ParagraphStyle("ct", fontName="UIB", fontSize=30, leading=36, textColor=NAVY)))
    s.append(Spacer(1, 4))
    s.append(Paragraph("Hydroponics Monitoring System – ESP32-S3-WROOM-1-N16R8", ParagraphStyle("cs", fontName="UI", fontSize=14, leading=19, textColor=TEAL)))
    s.append(Spacer(1, 14))
    s.append(P("Revision 3 – 22 October 2026 – project baseline. Supersedes Revision 2 (4 October 2026)."))
    s.append(Spacer(1, 8))
    s.append(table(["Item", "Value"], [
        ["Document role", "Architecture baseline for the whole firmware project. Every implementation, test and design change refers to the section, requirement (R), decision (D) and evidence (L/E) identifiers used here."],
        ["Target hardware", "ESP32-S3-WROOM-1-N16R8 (dual-core Xtensa LX7, 16 MB flash, 8 MB octal PSRAM) as drawn in the Sensor ↔ ESP32-S3 Signal Pinout of 22 October 2026, the Power Circuit Diagram and Redesign Specification Rev 5."],
        ["Software baseline", "PlatformIO espressif32@6.12.0, Arduino-ESP32 2.0.17 (board esp32-s3-devkitc-1, QIO flash + OPI PSRAM). Project code in C (gnu11) on the framework's C HAL and FreeRTOS kernel. NimBLE-Arduino 2.5.1, ArduinoJson 6.21.5."],
        ["Status", "Implementation has started (see firmware/README.md). Nothing here is hardware-validated unless stated; timing numbers are design budgets or acceptance targets unless identified as datasheet limits."],
        ["Preserved", "The inherited application, BLE demo and handover files are not edited by this document."],
    ], [0.22, 0.78]))
    s.append(Spacer(1, 8))
    s.append(H3("Change log"))
    s.append(table(["Revision", "Changes"], [
        ["Rev 1", "Architecture decision baseline: requirements R1–R10, decisions D01–D13, evidence L1–L13 and E1–E12."],
        ["Rev 2", "Same technical content, nothing removed. Restructured for readability: “Architecture at a glance” page, glossary, an “In short” summary per section, nine diagrams, worked examples (50 °C alarm, USB unplugged during a pH read, voltage-divider recovery, example safety rule). "
                  "Added: D14 (Safety on core 0 must be verified under radio load, with a defined fallback), D15 (single acquisition worker until stage 6), a new acceptance test for Safety latency under radio load, an open-items table, and a framework-version note on the references. Additions are marked with a purple <b>NEW IN REV 2</b> box or a purple table row."],
        ["Rev 3", "Hardware update; structure unchanged. Target is now the ESP32-S3-WROOM-1-N16R8 (D16 supersedes D02); project code is C (D17); the MQ-7 is replaced by an SGP30 on the retained 3.3 V supply (D18). "
                  "Signal map from the 22 October pin drawing, including the power sense (GPIO 8) and relay drive (GPIO 15); rail names from Redesign Specification Rev 5. New boxes and rows are green (<b>NEW IN REV 3</b>); S3, SGP30 and rail-name wording changes run throughout and are not individually marked."],
    ], [0.12, 0.88]))
    s.append(PageBreak())
    return s


def toc_page(toc):
    return [Paragraph("Contents", S["h1"]), Spacer(1, 4), toc, PageBreak()]


def glance():
    s = [H1("1. Architecture at a glance", "s1")]
    s.append(short(
        "Each sensor has its own driver file. One acquisition task on core 1 reads all sensors in turn every second (the BME280 every 250 ms), checks every value the moment it is read, and publishes a copied “latest snapshot”. "
        "Safety, power management, Wi-Fi/MQTT, BLE and configuration run on core 0 and never wait for a sensor. When external power disappears, a second acquisition task on core 0 takes over the four 3.3 V sensors and core 1 goes idle."))
    s.append(G.glance())
    s.append(caption("Figure 1 – Tasks, cores and the only channels through which they share data. Numbers in brackets are FreeRTOS priorities (higher = more urgent)."))
    s.append(H2("The six rules that hold the design together"))
    s.append(bullets([
        "<b>One owner per resource.</b> Exactly one task touches each sensor bus, the relay output, the MQTT client, the GATT server and NVS. Nobody else calls them; they send a request instead.",
        "<b>Copy, never share.</b> Tasks exchange fixed-size copied records through FreeRTOS queues. No pointers, Arduino <font name='Mono'>String</font> buffers or JSON views cross a task boundary.",
        "<b>Readers never wait for sensors.</b> BLE and MQTT read the latest snapshot immediately and report its age; a refresh request is asynchronous.",
        "<b>Check immediately, act locally.</b> Each valid sample is compared against its thresholds right after it is read; a critical value wakes Safety at once. Network reporting is secondary.",
        "<b>Power is a versioned fact.</b> A rail epoch counter (not only a boolean) tags every 5 V measurement, so a reading taken across a power cut is discarded instead of becoming a false alarm.",
        "<b>Everything is bounded.</b> Every wait has a timeout, every queue has a size and an overflow policy, every task is supervised by a watchdog or progress counter.",
    ], num=True))
    s.append(H2("Four decisions to understand first"))
    s.append(bullets([
        "Use the ESP32's supported FreeRTOS kernel explicitly. Creating our own tasks and schedules does not mean installing a second RTOS. Replacing the Espressif kernel would be a different project and conflicts with the selected Arduino/Wi-Fi/BLE stack.",
        "A BME280 has no threshold interrupt pin. Software can prioritize a critical value only after a measurement reveals it. Faster sampling reduces that blind interval; an independent threshold sensor is needed to eliminate dependence on polling.",
        "A one-second acquisition schedule is not a promise that every physical sensor produces a new, accurate value every second. SGP30 warm-up and baseline learning, DS18B20 conversion time and probe settling remain physical constraints.",
        "Low power follows the circuit's rails: BME280, SGP30, DS18B20 and DFR0026 stay on the retained 3.3 V supply; HC-SR04, pH and TDS lose their external 5 V supply and become unavailable. BLE remains available. The relay coil is fed from external 5 V, so the relay cannot be energized on battery.",
    ]))
    s.append(P("The proposed normal-mode allocation is acquisition and serial logging on core 1, with Wi-Fi, MQTT, BLE, power coordination and short safety actions on core 0. In low power, only the core-0 acquisition worker is active; the normal worker and logger remain blocked. "
               "This is single-core <i>application</i> execution, not runtime power removal from one CPU."))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def reading():
    s = [H1("2. How to read this document", "s2")]
    s.append(short("Sections 3–5 set the requirements, what we keep from the old code, and the hardware limits. Sections 6–14 define how the firmware works. Sections 15–19 cover low power, drivers, communications, faults and the delivery plan. Sections 20–22 hold the decision log and the sources behind every claim."))
    s.append(table(["Sections", "Topic", "Read it when you…"], [
        ["3–5", "Requirements, inherited code, hardware/pin contract", "need to know why something is required or which pin/rail a sensor uses"],
        ["6–8", "Interrupt limits and alarm latency, code layers, tasks and cores", "write a driver, create a task, or wonder where code belongs"],
        ["9–12", "1 s schedule, data model, synchronization, safety policy", "touch the acquisition loop, a queue, or an alarm rule"],
        ["13–15", "Power state machine, rail races and lease handover, low-power scope", "work on low-power mode or anything on the external 5 V branches"],
        ["16–18", "Sensor contracts, BLE/MQTT/configuration, fault tolerance", "bring up a sensor, change the protocol, or handle an error"],
        ["19–22", "Stages and acceptance tests, decision log, evidence and references", "plan work, review a change, or check a source"],
    ], [0.12, 0.45, 0.43]))
    s.append(Spacer(1, 6))
    s.append(H2("Conventions"))
    s.append(bullets([
        "<b>R1–R10</b> requirements (section 3), <b>D01–D18</b> decisions (section 20), <b>L1–L14</b> local evidence files (section 21), <b>E1–E13</b> manufacturer/software references (section 22). Brackets such as [L8, E2] point to the evidence behind a statement.",
        "“Budget” or “target” = a design number to be verified by measurement. “Datasheet limit” = a manufacturer figure.",
        "Priorities are FreeRTOS priorities: a higher number is more urgent. Stack sizes are in bytes as used by the ESP-IDF task API (1 KiB = 1024 bytes).",
        "GPIO numbers are ESP32-S3 GPIOs, not module pad numbers.",
        "Boxes: <font color='#12807a'><b>In short</b></font> = plain summary; <font color='#c07a2c'><b>Note</b></font> = caution; <font color='#2f5d8a'><b>Worked example</b></font> = step-by-step scenario; <font color='#7a3fa0'><b>New in Rev 2</b></font> and <font color='#2e7d32'><b>New in Rev 3</b></font> = added content.",
    ]))
    s.append(H2("Glossary"))
    g = [
        ["ADC1 / ADC2", "The ESP32-S3's two analog-to-digital converters. ADC2 is shared with Wi-Fi, so every analog input is on ADC1 (GPIO 1–10). This board uses GPIO 1, 6, 7 and 8."],
        ["Attenuation", "ADC input scaling. At the highest setting (12 dB, called 11 dB in older APIs) the ESP32-S3 measures up to about 3.1 V; signals above that saturate [E4]."],
        ["APP_CPU / PRO_CPU", "ESP32-S3 core 1 / core 0. Espressif's radio stacks normally run on core 0 (PRO_CPU)."],
        ["Task", "An independent FreeRTOS thread with its own stack, priority and (here) fixed core."],
        ["Task notification", "A lightweight FreeRTOS signal that wakes one specific task. It carries no measurement and cannot discover a sensor value."],
        ["Queue", "A FreeRTOS FIFO that copies fixed-size records between tasks. A length-one queue written with xQueueOverwrite() always holds only the newest record."],
        ["Snapshot", "The latest complete copy of all readings with their quality flags, ages and sequence numbers."],
        ["Sticky alarm", "An alarm bit that stays set until Safety handles it. A later normal reading cannot erase it."],
        ["Lease / generation", "Permission for exactly one acquisition worker to use the sensor buses. Each grant has a generation number so stale acknowledgements are recognized and ignored."],
        ["Quiesce", "Ask a task to stop starting new work, finish or cancel what is in progress, and acknowledge."],
        ["Rail epoch", "A counter incremented by the power ISR on every power-sense edge. A measurement is valid only if the epoch is unchanged from start to end."],
        ["ISR", "Interrupt service routine: minimal code that runs on a hardware event. No printing, I2C, allocation, JSON, Wi-Fi or delay calls."],
        ["Forced mode", "BME280 mode in which the host triggers one measurement, the sensor converts, then returns to sleep."],
        ["Oversampling ×1", "The BME280 takes one sample per measurement for temperature, pressure and humidity (written “x1/x1/x1” in Bosch tables)."],
        ["IIR filter", "BME280 internal smoothing filter; disabled initially so a step change is not delayed."],
        ["WCET", "Worst-case execution time: the longest a piece of code can take."],
        ["Jitter", "Variation of the actual start time of a periodic job around its ideal time."],
        ["Monotonic clock", "A time counter that only increases (e.g. microseconds since boot). Used for ages and deadlines, unlike NTP wall-clock time."],
        ["Task watchdog (TWDT)", "ESP-IDF watchdog that reports or resets if a subscribed task stops feeding it."],
        ["WAITI / idle", "When a core has no runnable task, its idle task executes the WAITI instruction and the core waits clock-gated. It is not powered off."],
        ["DFS / light sleep", "Dynamic frequency scaling and automatic light sleep; require an SDK built with power management (CONFIG_PM_ENABLE)."],
        ["Unicore", "CONFIG_FREERTOS_UNICORE: a build option that runs FreeRTOS on one core only. Cannot be switched at runtime."],
        ["NVS", "Non-volatile storage in flash (used by Preferences) for configuration."],
        ["GATT", "The BLE attribute protocol: services and characteristics (State, Telemetry, Command, Result, Auth)."],
        ["RMT", "ESP32-S3 remote-control peripheral, usable to time the HC-SR04 echo pulse in hardware."],
        ["CRC", "Checksum. The DS18B20 scratchpad carries one, and the SGP30 adds a CRC-8 to every 16-bit word, to detect corrupted transfers."],
        ["TVOC / CO2eq", "SGP30 outputs: total volatile organic compounds in ppb and an <i>estimated</i> CO2-equivalent in ppm derived from the same signal. CO2eq is not a CO2 measurement [E13]."],
        ["SGP30 baseline", "The sensor's internal compensation state. It is read back periodically, stored, and restored after a restart so readings do not have to re-learn it [E13]."],
        ["Hysteresis / debounce", "Different trip and clear thresholds / a required persistence time, both to prevent chatter."],
        ["Backoff", "Increasing wait between retries (here 1, 2, 4, 8 … up to 60 s with jitter)."],
        ["High-water mark", "The minimum free stack a task has ever had; used to size stacks from measurement."],
        ["PSRAM", "External RAM. The N16R8 module embeds 8 MB octal PSRAM, which occupies GPIO 35–37 [L14]. Task stacks, queues and ISR data stay in the 512 KB internal SRAM."],
        ["Strapping pin", "A pin sampled at reset to select boot mode or flash voltage. On the ESP32-S3: GPIO 0, 3, 45 and 46 [L14]."],
    ]
    s.append(table(["Term", "Meaning in this project"], g, [0.2, 0.8]))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def requirements():
    s = [H1("3. Requirements and interpretation", "s3")]
    s.append(short("Ten requirements come from the project request. Each has an architecture response and a way to accept it. The two most often misread are “sequential” (one task talks to the sensors at a time, but conversions may overlap) and “40 or 50 degrees” (examples, not commissioned thresholds)."))
    s.append(table(["ID", "Requirement", "Architecture response / acceptance"], [
        ["R1", "Explicit RTOS synchronization", "FreeRTOS tasks, bounded queues, notifications and one owner per peripheral. No application work in Arduino loop()."],
        ["R2", "Sequential polling every second", "Absolute one-second frame; one acquisition worker serializes driver operations. Conversion waits yield the CPU."],
        ["R3", "Critical readings receive priority", "Check each completed valid sample immediately; latch critical events and wake Safety. BME280 also runs every 250 ms."],
        ["R4", "Sensing and networking on different cores", "Normal acquisition on APP_CPU/core 1; network application tasks on PRO_CPU/core 0. Configure and inspect stack affinity."],
        ["R5", "BLE must not wait for sensors", "Read a copied latest snapshot; return its age, quality and availability immediately. A refresh request is asynchronous."],
        ["R6", "Approximately 3 V means normal; 0 V means low power", "Dedicated external-power-present input on GPIO 8 (220 Ω over 2 × 220 Ω from external 5 V, ≈ 3.33 V nominal), with ISR invalidation and debounce. It is not a battery-voltage measurement."],
        ["R7", "Low power retains 3.3 V sensors and BLE", "Disable external-5 V measurements and Wi-Fi/MQTT; hand acquisition of BME280, SGP30, DS18B20 and DFR0026 to the core-0 worker; keep core-1 application tasks blocked."],
        ["R8", "Fault tolerance", "Timeouts, independent safety state, stale-data detection, bounded storage, progress monitoring and explicit recovery."],
        ["R9", "Driver-by-driver development", "Dedicated PlatformIO bring-up environments use the same production drivers and a small test main."],
        ["R10", "Detailed plan before sensor implementation", "This PDF and editable source define interfaces, decisions and tests. Driver coding is the next stage."],
    ], [0.07, 0.28, 0.65]))
    s.append(H2("What “sequential” means"))
    s.append(P("Only one worker issues sensor transactions at a time. Start a conversion, return control, service another sensor and later collect the result. Sensors may convert internally at the same time; their bus transactions remain serialized. "
               "Waiting for every conversion to finish before touching another sensor would waste the one-second budget (the DS18B20 alone needs up to 750 ms)."))
    s.append(H2("Planning assumptions"))
    s.append(P("Retain PlatformIO and the Arduino-ESP32 framework from the submitted draft; project code is written in C (D17). Keep the baseline one-second schedule in both modes for enabled sensors. The BME280 fast interval also remains 250 ms in both modes; relaxing it for power savings requires a changed detection-latency requirement. "
               "BLE state/telemetry transport intervals are separate from sensing intervals."))
    s.append(note("Thresholds are not yet defined",
                  "Temperature values such as 40 or 50 degrees in the request are examples, not commissioned safety thresholds. Define real limits, hysteresis, actuator actions and reset rules before enabling automatic control. [L1–L4]"))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def inherited():
    s = [H1("4. Inherited code: keep, adapt or replace", "s4")]
    s.append(short("The old firmware is one Arduino loop in which slow network retries and blocking sensor reads delay each other, and drivers publish straight to MQTT. We start a new firmware/ directory, keep the old code untouched for reference, and port only algorithms whose units, wiring, timing and calibration have been checked."))
    s.append(table(["Evidence in the current files", "Decision and reason"], [
        ["Handover src/main.cpp:39, 72–117", "Replace the superloop/reconnect orchestration. HTTP GET and retry loops execute on the same path as sensing; MQTT retries delay and eventually restart the device."],
        ["src/ultraSonicSensor.cpp:25", "Replace default-timeout polling with capture + a sensor-specific deadline. pulseIn() is called without an explicit timeout (the 1 s default applies)."],
        ["src/waterTemp.cpp:11–15", "Retain the DS18B20 concept, replace its scheduling. It waits 100 seconds between updates and uses requestTemperatures() without asynchronous configuration."],
        ["src/sensor.cpp:15–32", "Remove MQTT publishing from drivers. Drivers return observations; transport services serialize and send them."],
        ["src/tdsSensor.cpp:36–65", "Rework sampling and compensation. One sample is inserted every 8 seconds, publication is every 5 seconds, and the initial buffer contains zeros. Comments disagree with executable intervals."],
        ["src/phSensor.cpp:16–17", "Reuse only as historical reference. Raw ADC scaling and hard-coded coefficients must become calibrated conversion stages, including the actual divider."],
        ["deviceManager.cpp / deviceFactory.cpp", "Keep stable device identities and supported-type concepts. Replace unrestricted runtime allocation and arbitrary pin changes with a bounded registry and validated board profiles."],
        ["ESP-Demo/src/Main.cpp:202–213", "Reuse the intent of setLowPower(): request Wi-Fi shutdown/restart and publish mode. Move actual changes into the coordinated services."],
        ["ESP-Demo/Main.cpp:451, 524, 536", "Keep callback-to-task command dispatch, but use a bounded pool or fixed records with explicit ownership, overflow results and session identity."],
        ["FirmwareContract.md", "Retain UUIDs, framing and authenticated commissioning concepts. Revise physical health, power policy and capabilities before connecting real sensors."],
    ], [0.33, 0.67]))
    s.append(H2("Recommended migration strategy"))
    s.append(P("Create a separate <font name='Mono'>firmware/</font> directory when implementation starts. Do not delete the old team code. Port algorithms only when their units, electrical assumptions, timing and calibration have been verified. "
               "Retain compatibility adapters for useful backend topic names and BLE operations instead of keeping transport code inside sensors."))
    s.append(P("The inherited climate implementation uses a DHT-family library; it is not a BME280 driver. The demo has no real sensor or relay drivers and reports a fixed simulated battery percentage. Neither is a production implementation of this plan. [L2–L4]"))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def hardware():
    s = [H1("5. Hardware boundary and pin contract", "s5")]
    s.append(short("Four sensors (BME280, SGP30, DS18B20, DFR0026) run from the retained 3.3 V supply and stay alive on battery. The HC-SR04 and the relay coil use the external 5 V supply, and pH and TDS a filtered branch of it; all three lose power when external power is removed. All analog inputs use ADC1 so they keep working with Wi-Fi on. "
                   "The power-sense input (GPIO 8) and the relay drive (GPIO 15) are now on the drawing; their features stay disabled until the hardware behavior has been measured."))
    s.append(P("The Power Circuit Diagram is the topology authority. The Sensor ↔ ESP32-S3 Signal Pinout (Pin Layout, 22 October 2026) defines the signal map, and Redesign Specification Rev 5 §3 restates it. These are design inputs, not proof that the physical assembly matches. [L5–L7]"))
    s.append(table(["Device / function", "Supply branch", "GPIO / interface", "Firmware consequence"], [
        ["BME280", "Retained 3.3 V", "SDA 21, SCL 47; I2C 0x76", "Forced measurement; verify chip ID. Shares the bus with the SGP30; 4.7 kΩ pull-ups to 3.3 V."],
        ["SGP30 module", "Retained 3.3 V", "SDA 21, SCL 47; I2C 0x58", "Measurement every 1 s, CRC on every word, WARMING for the first 15 s. The module must accept 3.3 V supply and logic."],
        ["DS18B20", "Retained 3.3 V", "DQ 14; 4.7 kΩ pull-up", "Externally powered 1-Wire; asynchronous conversion and CRC."],
        ["DFR0026 light", "Retained 3.3 V", "1 / ADC1_CH0; 10 kΩ series, 1 µF", "Raw/mV first; do not invent a lux conversion. The RC filter (τ ≈ 10 ms) limits how fast the input can follow."],
        ["SEN0244 TDS", "Filtered external 5 V", "6 / ADC1_CH5; 100 nF", "0–2.3 V nominal module output; water-temperature dependency."],
        ["pH-4502C", "Filtered external 5 V", "7 / ADC1_CH6; 10 kΩ / 15 kΩ, 1 µF", "Divider correction before calibration; DO/TO unconnected."],
        ["HC-SR04", "External 5 V", "TRIG 17, ECHO 18", "1 kΩ / 2 kΩ ECHO divider; capture timing; no-echo timeout. TRIG is driven at 3.3 V logic."],
        ["External-power present", "External 5 V (sense)", "8 / ADC1_CH7; 220 Ω over 2 × 220 Ω", "≈ 3.33 V at 5.00 V input. Digital input with an edge ISR; the ADC can confirm levels during commissioning."],
        ["Relay drive", "External 5 V coil", "15; IRLZ44N low side, 220 Ω gate, 10 kΩ pull-down, 1N5822 flyback", "Active-high. The pull-down holds the coil off through reset and boot. One SRD-05VDC relay; no coil supply on battery."],
        ["Native USB", "USB", "D− 19, D+ 20; 22 Ω series", "Programming and USB-Serial-JTAG console. Never reassign."],
        ["Branch-health feedback", "–", "Not on the drawing", "GPIO 8 senses the external feed, not the health of each 5 V branch."],
    ], [0.18, 0.15, 0.27, 0.40], hl3_rows=(1, 7, 8)))
    s.append(H2("Electrical limits that affect code"))
    s.append(bullets([
        "ADC1 (GPIO 1–10) avoids the ADC2/Wi-Fi conflict. GPIO 1, 6, 7 and 8 are used; GPIO 2, 4, 5, 9 and 10 remain free ADC1 inputs.",
        "GPIO 35–37 belong to the octal PSRAM on the N16R8 and must not be used. GPIO 19/20 are the native USB pair. The strapping pins are GPIO 0, 3, 45 and 46: the drawing puts the BOOT button on GPIO 0 and leaves 3, 45 and 46 unconnected. Review them before assigning anything new. [L6, L14]",
        "Recover module voltage as <b>V<sub>module</sub> = V<sub>adc</sub> × (R<sub>top</sub> + R<sub>bottom</sub>) / R<sub>bottom</sub></b>. Nominal factors are 25/15 for pH, 3/2 for the HC-SR04 echo and 660/440 for the power sense. Measure actual ratios.",
        "At its highest attenuation the ESP32-S3 ADC measures up to about 3.1 V [E4]. A 5 V pH output becomes 3.0 V at the pin: inside the range, but at its top, where non-linearity is largest. The DFR0026 is fed from 3.3 V and can exceed 3.1 V in bright light, which then saturates. Characterize both on the board; “below 3.3 V” is not an accuracy guarantee. [E4, L6]",
        "The ESP32-S3 input thresholds are V<sub>IH</sub> ≥ 0.75 × VDD (2.475 V) and V<sub>IL</sub> ≤ 0.25 × VDD (0.825 V), with V<sub>IH</sub> at most VDD + 0.3 V [L14]. The power-sense node is ≈ 3.33 V at 5.00 V and ≈ 3.5 V at 5.25 V, close to that ceiling; measure the actual feed voltage.",
    ]))
    s.append(example("Recovering the pH module voltage",
                     "The pH divider is 10 kΩ (top) over 15 kΩ (bottom). If the calibrated ADC reads 1.50 V, the module output is 1.50 × (10 + 15) / 15 = 2.50 V. "
                     "At the module's 5.0 V maximum, the pin sees 5.0 × 15 / 25 = 3.0 V – just inside the S3's ~3.1 V range, so the top of the pH range sits where the ADC is least linear. "
                     "For the power sense (220 Ω over 440 Ω): 5.00 V at the input gives 5.00 × 440 / 660 = 3.33 V at GPIO 8, well above the 2.475 V logic-high threshold."))
    s.append(new3("Hardware labels resolved",
                  "The power diagram and pin drawing now use IRLZ44N for both the source-selection stage and the relay driver, with 1N5822 diodes, and show one SRD-05VDC relay. The earlier “IRLZ440N” label and the IRF4905 of Redesign Specification Rev 4 are superseded. "
                  "The BLE demo catalog can still describe four relays; the board has one. [L3, L5–L7]"))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def interrupts():
    s = [H1("6. Interrupts and realistic alarm latency", "s6")]
    s.append(short("No sensor on this board can raise an interrupt when a value becomes critical, so “priority” is earned by checking every reading immediately and by reading the BME280 four times per second. The worst-case software reaction to a sustained over-temperature is about 277 ms (target 300 ms). Real interrupts are used for power-sense edges and echo timing, not for sensor values."))
    s.append(H2("Observation comes before priority"))
    s.append(P("A task notification is a software scheduling mechanism. It cannot discover a sensor value that has not been measured. The BME280 pin table has no alarm/INT output. The DS18B20 alarm-search command also requires host bus activity after a conversion; it is not an asynchronous interrupt pin. "
               "The pH module's DO comparator output is unconnected in the signal drawing, and the SGP30 has no interrupt output. [L6, L8–L9]"))
    s.append(P("Three mechanisms are therefore separate:"))
    s.append(bullets([
        "<b>Immediate check.</b> Immediately after each valid reading, evaluate the configured threshold, update a sticky pending-alarm record and notify Safety. Do not wait for the one-second snapshot boundary.",
        "<b>Fast lane.</b> Poll BME280 every 250 ms using forced mode, oversampling ×1 for temperature, pressure and humidity, and no IIR filter delay initially. Keep ordinary sensors on the one-second schedule.",
        "<b>Independent hardware (optional).</b> If an independently bounded temperature response is essential, add a suitable temperature switch or sensor with a hardware alert output.",
    ], num=True))
    s.append(H2("Example latency budget – not a measurement"))
    s.append(P("For a sustained condition observable by the BME280, the proposed software detection/action bound is:"))
    s.append(code("250 ms sample interval + 5 ms scheduling target + 12 ms conversion/read budget\n+ 10 ms Safety target = 277 ms;  round acceptance target to 300 ms."))
    s.append(G.latency())
    s.append(caption("Figure 2 – Worst case: the temperature crosses the limit just after a sample, so it is seen at the next one 250 ms later."))
    s.append(P("The Bosch maximum conversion formula at oversampling ×1/×1/×1 gives 1.25 + 2.3 + (2.3 + 0.575) + (2.3 + 0.575) = 9.3 ms. The 12 ms slot includes a small bus margin; verify it on the chosen I2C setup. "
               "Sensor thermal response, enclosure placement, filtering and actuator mechanics are additional delays. A short excursion entirely between samples may be missed. [L8, section 9.1]"))
    s.append(example("Air temperature rises to 50 °C",
        "<b>t = 0 ms</b> – AcquisitionNormal reads the BME280: 20 °C, OK. It continues with DS18B20, HC-SR04 and the analog channels.<br/>"
        "<b>t ≈ 1 ms</b> – the air actually crosses the configured limit. Nothing can see it yet: the BME280 has no interrupt pin.<br/>"
        "<b>t = 250 ms</b> – the fast-lane deadline interrupts the ordinary order between bounded operations. Forced measurement + read (≤ 12 ms) returns 50 °C.<br/>"
        "<b>t ≈ 262 ms</b> – in the same step, the pure threshold function trips: the sticky alarm bit for BME280 temperature is set, a detail record is queued and Safety (core 0, priority 6) is notified.<br/>"
        "<b>t ≤ 277 ms</b> – Safety takes the configured local action (≤ 10 ms target). The snapshot is published; BLE and MQTT report the alarm afterwards.<br/>"
        "For comparison, the old firmware read climate every 5 s inside a loop that could block for seconds, so the same event could go unseen for 5 s or more."))
    s.append(H2("Genuine interrupt uses"))
    s.append(P("External-power edges on GPIO 8 invalidate measurements immediately and wake PowerCoordinator. HC-SR04 echo edges or RMT capture indicate measurement completion, not a critical-value alarm. "
               "Optional comparator wiring needs confirmed voltage levels, polarity, hysteresis and meaningful calibration; the pH module's DO pin is not a calibrated pH alarm."))
    s.append(P("ISRs only capture minimal state and use ISR-safe notification APIs. No printing, I2C, allocation, JSON, Wi-Fi or delay calls. Install the power ISR on core 0 and verify its code/data residency (internal RAM, not PSRAM or flash) and interrupt behavior in the selected SDK. [E2]"))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def layered():
    s = [H1("7. Layered code instead of a large main", "s7")]
    s.append(short("Keep the habit of one .h/.c pair per sensor – that is the driver layer. What replaces the “big main” is two more layers: services that schedule and decide, and transports that talk to the network. main.c only boots the system. Each sensor also gets its own small test main as a separate PlatformIO environment."))
    s.append(code(
        "firmware/\n"
        "  platformio.ini            pinned build + test_<sensor> environments\n"
        "  include/board_profile.h   pins, rails, capabilities; unresolved = disabled\n"
        "  include/app_config.h      periods, queue sizes, budgets, policy defaults\n"
        "  src/main.c                bootstrap only\n"
        "  src/drivers/              bme280, sgp30, ds18b20, light, hcsr04, ph, tds\n"
        "  src/hal/                  I2C, 1-Wire, ADC calibration, capture, GPIO\n"
        "  src/domain/               readings, units, alarms, policy, calibration\n"
        "  src/services/             acquisition, safety, power, configuration\n"
        "  src/transports/           wifi_mqtt, ble, protocol adapters\n"
        "  src/diagnostics/          progress counters, reset cause, buffered logs\n"
        "  test/bringup/             one small entry point per sensor\n"
        "  test/native/              policy, scheduling, frame and transition tests"))
    s.append(G.layers())
    s.append(caption("Figure 3 – The three layers below a bootstrap-only main.c."))
    s.append(H2("Rules between layers"))
    s.append(bullets([
        "<b>Drivers</b> know hardware and report status. They do not publish, select system power mode, own arbitrary RTOS tasks or change a relay as a side effect of reading a sensor. The HAL may internally use bounded SDK waits or capture interrupts; the sensor API does not expose networking or service state.",
        "<b>Domain</b> functions perform unit conversion, validity checks and threshold decisions using explicit inputs (pure functions, testable on a PC under test/native/).",
        "<b>Services</b> schedule work and own resources.",
        "<b>Transports</b> consume copied data and submit commands. No driver includes MQTT/BLE headers or depends on a global client.",
        "Project code is C (D17): one .h/.c pair per module. A driver is a struct whose first member is the common <font name='Mono'>SensorDriver</font>, holding a table of operation pointers, in place of a C++ class. The framework's C++ libraries (NimBLE-Arduino, ArduinoJson) are used only through a thin adapter, or replaced by NimBLE's C host API and cJSON/snprintf. One file pair per sensor is useful; <b>one task per sensor is unnecessary</b> for this sequential system.",
    ]))
    s.append(H2("Thin main responsibilities (in order)"))
    s.append(bullets([
        "Set safe output defaults before network startup.",
        "Load and validate board/configuration data.",
        "Create static queues and workers.",
        "Sample the initial power state.",
        "Start Safety and PowerCoordinator; then grant acquisition ownership.",
        "Initialize communications without waiting for an access point. Initialization failure enters a reported safe fault state.",
    ], num=True))
    s.append(P("Arduino's loop() remains a compatibility stub that blocks indefinitely, with loop-task watchdog subscription disabled/removed explicitly; idle watchdogs remain enabled. No sensing, reconnect or policy work is placed there. "
               "Do not remove the framework kernel or blindly delete its loop task. setup() and loop() are written in C and bound to the symbols the Arduino core calls."))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s


def tasks():
    s = [H1("8. Task and core allocation", "s8")]
    s.append(short("Nine application tasks: two on core 1 (normal acquisition and the serial logger) and seven on core 0. Safety and power coordination stay on core 0 in both modes so the relay controller never migrates. The low-power acquisition worker already exists on core 0 and simply waits until it is granted the sensors."))
    s.append(P("These are initial application priorities, not guarantees against higher-priority SDK radio tasks. A higher number is higher priority. Stack reservations are starting budgets in bytes for ESP-IDF task APIs and must be measured with both radios active. [E2]"))
    s.append(table(["Task", "Core / priority", "Trigger and ownership", "Initial stack"], [
        ["Safety", "0 / 6", "Alarm/control notification; 100 ms health check. Sole relay-output owner; short, bounded actions.", "3 KiB"],
        ["PowerCoordinator", "0 / 5", "Power edges / transition acknowledgements. Owns mode and acquisition lease.", "4 KiB"],
        ["AcquisitionNormal", "1 / 4", "Absolute deadlines; all sensor buses in normal mode. Blocked in low power.", "4 KiB"],
        ["AcquisitionLowPower", "0 / 4", "Same engine, 3.3 V devices only. Blocked in normal mode.", "4 KiB"],
        ["WiFiMqtt", "0 / 2", "Connection events, retries, latest data. Sole Wi-Fi/MQTT application owner.", "6 KiB"],
        ["BleTransport", "0 / 2", "Bounded RX/TX queues, state and telemetry deadlines. Sole application GATT sender.", "6 KiB"],
        ["Configuration", "0 / 2", "Validated commands and persistence. Sole registry/NVS writer.", "4 KiB"],
        ["Supervisor", "0 / 1", "Every 1 s: progress, queue usage, stack/heap and health.", "3 KiB"],
        ["SerialLogger", "1 / 1", "Bounded log queue; normal mode only. Quiesces in low power.", "2 KiB"],
    ], [0.22, 0.14, 0.52, 0.12]))
    s.append(Spacer(1, 6))
    s.append(G.modes())
    s.append(caption("Figure 4 – Which tasks run in each mode. Dashed = created but blocked; it consumes stack, not CPU."))
    s.append(P("Keeping Safety on core 0 in both modes avoids migrating an actuator controller. Its handler must never wait for network I/O or NVS. Most calculations remain in acquisition on core 1 in normal mode. This is an intentional exception to the simple “all sensing logic on one core” rule."))
    s.append(P("The two acquisition tasks execute the same engine and share a context only through an exclusive lease. Only the granted worker can access it. The inactive worker blocks indefinitely on a grant queue; it does not poll or touch the drivers. "
               "Budget about 36 KiB for these application stacks, plus queues, driver state and substantial SDK/BLE/network memory. Keep stacks, queues and ISR-visible data in internal SRAM. "
               "The 8 MB PSRAM suits large buffers such as the telemetry backlog or BLE frame pool, but only after its access latency is measured and nothing in it is touched from an ISR."))
    s.append(P("Wireless controller and host tasks belong to the SDK/libraries. Confirm their actual affinity; pinning BleTransport does not itself pin the NimBLE host. Configure the host using the selected library's supported build setting and inspect task traces before claiming core 1 has no application work in low power. [E1–E2, E6–E8]"))
    s.append(new("D14: Safety shares core 0 with the radio stacks",
        "SDK tasks for Wi-Fi, lwIP (TCP/IP) and the NimBLE host also run on core 0, typically at FreeRTOS priorities around 18–23 – well above Safety's 6. A burst of radio work can therefore delay Safety even though it is the most urgent <i>application</i> task. "
        "The ≤ 10 ms sample-to-Safety-action target (section 19) must be measured <b>with Wi-Fi and BLE under heavy traffic</b>, not on an idle radio. Read the actual SDK priorities from a task trace of the resolved build. "
        "<b>Fallback if the test fails:</b> run Safety on core 1 in normal mode (beside AcquisitionNormal, at a priority above it) and hand it to core 0 only in low power, through the same quiesce/grant protocol as acquisition (section 14)."))
    s.append(CondPageBreak(PAGE_H * 0.42))
    return s
