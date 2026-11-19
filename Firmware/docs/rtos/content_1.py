# Front matter and sections 1-5: purpose, FreeRTOS primer, platform facts, tasks, boot.
from rtkit import *
import figs as G
from reportlab.platypus import PageBreak, CondPageBreak, NextPageTemplate
from reportlab.lib.styles import ParagraphStyle


def front():
    s = [NextPageTemplate("body")]
    s.append(Spacer(1, 50 * mm))
    s.append(Paragraph("RTOS Architecture", ParagraphStyle("ct", fontName="UIB", fontSize=30, leading=36, textColor=NAVY)))
    s.append(Spacer(1, 4))
    s.append(Paragraph("Tasks, scheduling, channels, Safety and supervision – Stage 4 design (WP7 + WP10)",
                       ParagraphStyle("cs", fontName="UI", fontSize=13, leading=18, textColor=TEAL)))
    s.append(Spacer(1, 14))
    s.append(P("Revision 1 – 25 October 2026. Companion to <i>Firmware Architecture Baseline</i> Rev 3 (22 October 2026)."))
    s.append(Spacer(1, 8))
    s.append(table(["Item", "Value"], [
        ["Purpose", "Explain, in implementation detail, how the firmware uses FreeRTOS: which tasks exist, on which core and at which priority, how the one-second schedule is executed, how data and alarms move between tasks, how Safety drives the grow-lamp relay, and how stalls are detected. It is the reference for implementing and reviewing WP7 and WP10."],
        ["Readers", "Every team member who writes or reviews firmware. Section 2 is a FreeRTOS primer for readers new to it; the rest assumes it."],
        ["Authority", "Firmware Architecture Rev 3 remains the design authority. This document refines its sections 8–12 and 18 for Stage 4 and cites them as §n. Where the two differ, the difference is stated and explained here (for example the analog filter windows in section 6.3)."],
        ["Status", "<b>This is a design, mostly not yet implemented.</b> Every task, file and channel carries a status tag: "
                   + tag("done") + " exists in the code today, " + tag("plan") + " is built in this milestone, "
                   + tag("later") + " belongs to WP8 or WP9, " + tag("name") + " is the intended name of a file or symbol that does not exist yet."],
        ["Numbers", "Measured values are labelled <i>measured</i> and come from firmware/README.md bring-up records. Everything else is a design budget or an acceptance target, not a measurement."],
        ["Target", "ESP32-S3-WROOM-1-N16R8, PlatformIO espressif32@6.12.0, Arduino-ESP32 2.0.17 (ESP-IDF 4.4 based), project code in C."],
    ], [0.18, 0.82]))
    s.append(Spacer(1, 8))
    s.append(H3("Change log"))
    s.append(table(["Revision", "Changes"], [
        ["Rev 1", "First issue. Written at the start of Stage 4, after the six sensor drivers (WP1–WP6) were completed and tested on hardware."],
        ["Rev 1.1", "Stage 4 implemented and run on S3 hardware (25 Oct 2026): DS18B20 timing corrected to start-then-read in the same frame (section 6.3); logger record 128 characters; file map tags updated. Most tags in sections 1–13 still read STAGE 4 = built in this milestone; it is now built. Measured results (schedule, lamp, stall tests, stacks, heap) are in firmware/README.md \"Production run\"."],
    ], [0.12, 0.88]))
    s.append(PageBreak())
    return s


def toc_page(toc):
    return [Paragraph("Contents", S["h1"]), Spacer(1, 4), toc, PageBreak()]


# ---------------------------------------------------------------- 1
def about():
    s = [H1("1. The design on one page", "r1")]
    s.append(short(
        "Four application tasks run in Stage 4. <b>AcqNormal</b> (core 1) is the only code that touches the sensors: it runs every driver on an absolute-deadline schedule – a one-second frame, plus the BME280 every 250 ms – and publishes a copied snapshot after every completed reading. "
        "<b>Safety</b> (core 0, highest application priority) is the only code that drives the relay: it wakes on an alarm event or every 100 ms, latches alarms, and switches the grow lamp from the light reading – OFF whenever that reading is stale or faulty. "
        "<b>SerialLogger</b> and <b>Supervisor</b> make the system observable and catch stalls without ever blocking the first two."))
    s.append(G.cores())
    s.append(caption("Figure 1 – Tasks per core after Stage 4. Blue = built in this milestone; dashed grey = later work packages."))
    s.append(H2("The eight rules everything else follows from"))
    s.append(table(["#", "Rule", "Consequence in code"], [
        ["1", "One owner per resource.", "Only AcqNormal calls drivers or bus HALs. Only Safety writes GPIO15. Only SerialLogger writes the UART. No mutex is needed around any of them."],
        ["2", "Readers never wait for a sensor.", "The latest values live in a length-1 queue that the writer overwrites and readers peek with a zero timeout."],
        ["3", "Every wait is bounded or event-driven.", "Tasks block on a task notification with a timeout. Nothing spins, nothing sleeps a full second, nothing waits on the network."],
        ["4", "Copies, not pointers, cross task boundaries.", "Queues carry fixed-size records by value (Reading 56 B, Snapshot 584 B). No pointer into another task's memory is ever shared."],
        ["5", "Alarms are sticky; detail is best-effort.", "A per-rule sticky slot survives a full detail queue; the queue only adds history and counts what it dropped."],
        ["6", "Stale data is unsafe data.", "Safety re-checks reading age on every 100 ms wake, so a silent sensor or a stalled task turns the lamp OFF."],
        ["7", "Decisions are pure functions.", "Schedule, alarm and Safety logic live in src/domain (no FreeRTOS headers) and are unit-tested on the PC with injected time."],
        ["8", "Allocate once, at boot.", "Every task stack, queue and buffer is static. Nothing calls malloc after setup()."],
    ], [0.04, 0.3, 0.66]))
    s.append(H2("What exists today and what this milestone adds"))
    s.append(table(["Area", "Today", "After Stage 4"], [
        ["Drivers + HAL", done("All seven drivers (BME280, DS18B20, DFR0026, HC-SR04, pH, TDS, SGP30), I2C, 1-Wire, ADC, pulse-capture ISR. Each runs in its own bring-up env."), "Unchanged, plus the audit fixes listed in section 15."],
        ["Production main.c", done("Applies (empty) safe defaults, logs the board gate, creates the snapshot queue, returns."), plan("Starts Safety, AcqNormal, Supervisor and SerialLogger in a defined order.")],
        ["Scheduling", done("Bring-up harness: one driver per task, vTaskDelayUntil. test_water / test_climate interleave two drivers."), plan("One multi-rate engine for all seven drivers with absolute deadlines.")],
        ["Safety / relay", done("domain/lamp_control decides; test_lamp drives the relay itself."), plan("Safety task owns the relay and runs the lamp policy with staleness → OFF.")],
        ["Logging", done("logPrintf() calls printf directly (blocking)."), plan("32-entry queue, SerialLogger task, drop-and-count.")],
        ["Supervision", done("Stubs only."), plan("Progress deadlines, stack / heap checks, task watchdog subscriptions.")],
        ["Power modes, transports", done("rail_state record with portMUX; power sense measured on GPIO8."), later("WP9 lease handover and second worker; WP8 BLE / MQTT / NVS.")],
    ], [0.17, 0.43, 0.4]))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 2
def primer():
    s = [H1("2. FreeRTOS on the ESP32-S3 – what you need to know", "r2")]
    s.append(short(
        "FreeRTOS gives each task its own stack and runs the highest-priority task that is ready, preempting lower ones at once. The ESP32-S3 runs two FreeRTOS schedulers, one per core, over shared memory. "
        "Tasks spend almost all their time <i>Blocked</i> on a queue, a notification or a delay; that is what makes a 1 ms tick and two cores enough for everything here."))
    s.append(H2("2.1 Tasks and their states"))
    s.append(P("A task is a C function that never returns, with its own stack and a priority. The scheduler keeps every task in one of four states:"))
    s.append(table(["State", "Meaning", "Example in this firmware"], [
        ["Running", "Executing on a core right now. At most one task per core.", "AcqNormal while it reads the BME280."],
        ["Ready", "Could run, but a higher- or equal-priority task holds the core.", "Supervisor while Safety runs on core 0."],
        ["Blocked", "Waiting for an event or a timeout. Uses no CPU at all.", "Safety in ulTaskNotifyTake(…, 100 ms); AcqNormal between ADC taps."],
        ["Suspended", "Removed from scheduling until explicitly resumed. Not used here.", "–"],
    ], [0.14, 0.46, 0.4]))
    s.append(P("The project's tasks are all created with " + m("xTaskCreateStaticPinnedToCore()") + ": the stack and the task control block are static arrays, and the task is fixed to one core. A task function is an endless loop whose first statement is a blocking wait."))
    s.append(code("static StaticTask_t s_tcb;\n"
                  "static StackType_t  s_stack[3 * 1024];          // bytes on ESP-IDF (not words)\n\n"
                  "static void safetyTask(void* arg) {\n"
                  "  for (;;) {\n"
                  "    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CFG_SAFETY_HEALTH_CHECK_MS));  // Blocked here\n"
                  "    safetyPass(monoNowMs());                                           // short, bounded\n"
                  "  }\n"
                  "}\n\n"
                  "const TaskSpec* t = cfgTaskSpec(TASK_ID_SAFETY);  // name, core, priority, stack\n"
                  "xTaskCreateStaticPinnedToCore(safetyTask, t->name, t->stackBytes, NULL,\n"
                  "                              t->priority, s_stack, &s_tcb, t->core);"))
    s.append(note("ESP-IDF stack sizes are in bytes",
        "Vanilla FreeRTOS counts stack depth in words; ESP-IDF counts bytes. " + m("cfgTaskSpec()") + " already stores bytes (3 * 1024 for Safety). Do not multiply by 4."))
    s.append(H2("2.2 Priorities and preemption"))
    s.append(P("Priorities run from 0 (idle) to 24 on this build; a higher number wins. When a higher-priority task becomes Ready – because its notification arrived or its timeout expired – it preempts the running task on that core <b>immediately</b>, not at the next tick. Tasks of equal priority on the same core share it in 1 ms time slices."))
    s.append(P("Priority is only meaningful <i>within a core</i>. Safety (6) on core 0 and AcqNormal (4) on core 1 never compete: each core has its own scheduler. What matters for Safety is which other tasks share core 0 – in Stage 4 only Supervisor (1) and SDK tasks; from WP8 on also the Wi-Fi, lwIP (18) and NimBLE tasks, which outrank it (decision D14, section 10.3)."))
    s.append(H2("2.3 Two cores, pinned tasks"))
    s.append(P("ESP-IDF FreeRTOS is symmetric multiprocessing: one kernel, two schedulers, shared RAM. A task can be pinned to core 0, pinned to core 1, or left unpinned (" + m("tskNO_AFFINITY") + "). <b>Every project task is pinned</b> (decision D07): the timing of a pinned task is reproducible, an ISR installed by it stays on that core, and its state never migrates mid-operation."))
    s.append(table(["Core", "Convention on ESP32", "Our use"], [
        ["Core 0 (PRO_CPU)", "Wi-Fi, Bluetooth controller, lwIP and most SDK services.", "Safety, Supervisor; later PowerCoordinator, transports, Configuration."],
        ["Core 1 (APP_CPU)", "Arduino loopTask and application code.", "AcqNormal and SerialLogger. In low power (WP9) their work stops and core 1 only idles."],
    ], [0.18, 0.42, 0.4]))
    s.append(H2("2.4 Time: the tick and the monotonic clock"))
    s.append(P("The scheduler tick is 1 kHz (" + m("CONFIG_FREERTOS_HZ=1000") + "), so " + m("pdMS_TO_TICKS(ms)") + " is exact and a timeout has 1 ms resolution. A timeout of N ticks wakes after between N−1 and N ms, because the current tick is already partly over: deadlines below 2 ms are not meaningful as delays."))
    s.append(P("All ages and deadlines use " + m("monoNowMs()") + " (" + m("esp_timer_get_time() / 1000") + ", a 64-bit microsecond counter that does not wrap in practice), never the tick count and never wall-clock time. The tick count is only used inside the conversion to a timeout."))
    s.append(H2("2.5 Waiting: delays, notifications and queues"))
    s.append(table(["Primitive", "What it does", "Where we use it"], [
        ["vTaskDelay(n)", "Block for n ticks from now. Drifts: the work before it adds to the period.", "Only the bring-up harness inside a poll loop. Not in production tasks."],
        ["vTaskDelayUntil(&amp;last, n)", "Block until last + n ticks: an absolute period without drift. Cannot be woken early.", "Bring-up harness. Rejected for the production engine (section 6.5)."],
        ["ulTaskNotifyTake(clear, timeout)", "Block until another task or an ISR calls xTaskNotifyGive / vTaskNotifyGiveFromISR, or until the timeout. The cheapest wake-up FreeRTOS has: no separate object, 4 bytes in the TCB.", "AcqNormal (timeout = next deadline), Safety (timeout = 100 ms), SerialLogger and Supervisor."],
        ["xQueueSend / Receive", "Copy a fixed-size item in or out of a bounded FIFO. Optional timeout when full / empty.", "Alarm detail queue, log queue. Always with a zero timeout on the sending side."],
        ["xQueueOverwrite / Peek", "Length-1 queue used as a mailbox: the writer replaces the single item; readers copy it without removing it.", "Snapshot store, SafetyStatus mailbox."],
    ], [0.24, 0.42, 0.34]))
    s.append(why("Copy semantics",
        "A FreeRTOS queue copies the item <i>into</i> its own storage on send and <i>out</i> on receive. After " + m("xQueueOverwrite(q, &amp;snap)") + " returns, the writer may change " + m("snap") + " freely; a reader holds its own private copy. "
        "That is why no lock is needed around a Snapshot. The price is the copy time: 584 bytes is about 1–2 µs at 240 MHz, negligible next to any sensor operation."))
    s.append(H2("2.6 Shared data between cores: portMUX critical sections"))
    s.append(P("When two tasks – possibly on different cores – or a task and an ISR must update the same small record, the update is wrapped in a spinlock critical section:"))
    s.append(code("static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;\n\n"
                  "portENTER_CRITICAL(&s_mux);        // task side; _ISR variants inside interrupts\n"
                  "slot->tripped = true; slot->count++; if (v > slot->worst) slot->worst = v;\n"
                  "portEXIT_CRITICAL(&s_mux);"))
    s.append(P("Inside the section interrupts are disabled on this core and the other core spins if it tries to take the same lock. So the section must be a few instructions long: <b>no I/O, no printf, no FreeRTOS call that can block, no loop over a sensor</b>. rail_state.c already uses this pattern for the {valid, epoch} pair " + tag("done") + "."))
    s.append(rule("volatile is not synchronisation",
        m("volatile") + " stops the compiler caching a variable in a register; it does not make a multi-field update atomic and does not order memory between cores. "
        "A 64-bit " + m("MonoMs") + " is written as two 32-bit halves, so a reader on the other core can see a torn value. Use a portMUX section (progress stamps, sticky alarm slots) or a queue copy."))
    s.append(H2("2.7 Interrupts"))
    s.append(P("An ISR runs on the core that installed it, preempts every task there, and must finish in microseconds. In this design ISRs only record a timestamp or bump a counter and, if needed, notify a task with " + m("vTaskNotifyGiveFromISR()") + " followed by " + m("portYIELD_FROM_ISR()") + "."))
    s.append(table(["ISR rule", "Reason"], [
        ["Mark the handler and everything it calls IRAM_ATTR.", "Keeps the handler fast and cache-independent. Note: Arduino 2.0.17 installs its GPIO ISR service without ESP_INTR_FLAG_IRAM (CONFIG_ARDUINO_ISR_IRAM not set), so GPIO interrupts are held off while the flash cache is disabled (NVS / OTA writes) and an edge is serviced late rather than crashing. Any interrupt registered as IRAM-safe must have all its code in IRAM (audit L3)."],
        ["No printf, no I2C / ADC, no malloc, no blocking call.", "None of them are ISR-safe; most take locks."],
        ["Read GPIO levels by register, not digitalRead().", "digitalRead() lives in flash; pulse_capture.c already does this " + tag("done") + "."],
        ["Only *FromISR APIs, then yield if a higher task woke.", "Otherwise the woken task waits for the next tick."],
    ], [0.45, 0.55]))
    s.append(H2("2.8 Memory: static allocation, internal RAM and PSRAM"))
    s.append(P("The N16R8 has about 512 KiB of internal SRAM and 8 MiB of PSRAM. In this framework build " + m("malloc()") + " of more than 4 KiB may return PSRAM (" + m("CONFIG_SPIRAM_USE_MALLOC=y") + ", " + m("ALWAYSINTERNAL=4096") + "). PSRAM is slower, is unreachable while the flash cache is off, and must never hold a task stack or ISR data. "
               "Static arrays are always placed in internal RAM, so the rule <i>allocate statically at boot</i> also settles the PSRAM question (decision D16)."))
    s.append(H2("2.9 Watchdogs"))
    s.append(table(["Watchdog", "What it catches", "Configuration (this build)"], [
        ["Interrupt watchdog", "An ISR or critical section that holds a core too long.", "300 ms, both CPUs. Panic and reset."],
        ["Task watchdog (TWDT)", "A subscribed task that has not called esp_task_wdt_reset() within the timeout.", "5 s, panic. IDLE0 is subscribed by the SDK; IDLE1 is not."],
        ["Progress stamps (ours)", "A task that is alive but stuck in a wait or a retry loop, before the TWDT fires.", "Supervisor, every 1 s (section 11)."],
    ], [0.2, 0.44, 0.36]))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 3
def platform():
    s = [H1("3. Platform facts this design relies on", "r3")]
    s.append(short(
        "The values below were read from the resolved framework package (framework-arduinoespressif32 3.20017.241212, tools/sdk/esp32s3/sdkconfig) on 25 October 2026. "
        "The README build record of 4 October quotes the original ESP32 sdkconfig; where the two differ, this table is the S3 truth."))
    s.append(table(["Setting", "Value", "What it means for us"], [
        ["CONFIG_FREERTOS_UNICORE", "not set", "Two cores, two schedulers. A single-core image would be a different build, not a runtime switch."],
        ["CONFIG_FREERTOS_HZ", "1000", "1 ms tick; pdMS_TO_TICKS is exact."],
        ["CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ", "240", "<b>The CPU runs at 240 MHz.</b> CFG_CPU_MHZ_NORMAL = 160 in app_config.h is not applied anywhere (audit D9); WP9 decides the frequency policy."],
        ["CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION", "y", "xTaskCreateStaticPinnedToCore, xQueueCreateStatic available."],
        ["CONFIG_FREERTOS_CHECK_STACKOVERFLOW", "CANARY", "Overflow detected at context switch by a canary word; reset with a 'stack overflow in task X' message."],
        ["CONFIG_FREERTOS_ISR_STACKSIZE", "2096 B", "Shared interrupt stack: keep ISRs shallow."],
        ["CONFIG_FREERTOS_IDLE_TASK_STACKSIZE", "1024 B", "No idle hooks in this project."],
        ["CONFIG_FREERTOS_TIMER_TASK_PRIORITY", "1 (stack 3120 B)", "Software-timer daemon exists but is unused by the application."],
        ["CONFIG_ESP_TASK_WDT_TIMEOUT_S", "5", "Equals CFG_TASK_WATCHDOG_MS. The Arduino core already initialises the TWDT; we only add our tasks."],
        ["CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 / CPU1", "y / not set", "Starvation of core 0 is caught by the SDK; starvation of core 1 is <b>not</b> – our progress stamps cover AcqNormal instead."],
        ["CONFIG_ESP_INT_WDT_TIMEOUT_MS", "300", "Any critical section or ISR longer than 300 ms resets the chip."],
        ["CONFIG_ARDUINO_RUNNING_CORE", "1 (stack 8192 B)", "loopTask runs setup() and loop() on core 1, priority 1. Our loop() blocks forever."],
        ["CONFIG_ESP_MAIN_TASK_AFFINITY", "CPU0", "app_main starts on core 0 and exits after creating loopTask."],
        ["CONFIG_ESP_IPC_TASK_STACK_SIZE", "1024 B", "ipc0 / ipc1 tasks at priority 24 run cross-core calls (e.g. installing an interrupt on the other core)."],
        ["CONFIG_ESP_TIMER_TASK_STACK_SIZE", "4096 B", "esp_timer callbacks run in a high-priority task (22 in IDF 4.4 – verify by trace); we only read esp_timer_get_time()."],
        ["CONFIG_LWIP_TCPIP_TASK_PRIO / AFFINITY", "18 / CPU0", "From WP8: the TCP/IP task outranks Safety on core 0 (D14)."],
        ["CONFIG_ESP32_WIFI_TASK_PINNED_TO_CORE_0, BT_CTRL_PINNED_TO_CORE", "y / 0", "Radio work stays on core 0. NimBLE-Arduino pins its host task to core 0 (nimconfig.h)."],
        ["CONFIG_SPIRAM_USE_MALLOC, ALWAYSINTERNAL", "y, 4096", "Large malloc may land in PSRAM – one more reason for static buffers."],
        ["CONFIG_PM_ENABLE", "not set", "No automatic frequency scaling or light sleep with this framework (§15)."],
        ["CONFIG_ESP_CONSOLE_UART_NUM / BAUDRATE", "0 / 115200", "printf goes to UART0, mirrored to USB-Serial-JTAG; lines end CRLF."],
    ], [0.36, 0.14, 0.5]))
    s.append(P("PlatformIO's " + m("memory_type = qio_opi") + " selects the octal-PSRAM variant of the SDK headers at build time; the base sdkconfig above shows quad PSRAM, which does not apply to our board. None of the RTOS settings listed differ between the variants."))
    s.append(note("Re-check after any framework change",
        "These values change with the platform version. After a platform bump, re-read the S3 sdkconfig and update this table before relying on the timing in later sections."))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 4
def tasks():
    s = [H1("4. Task inventory", "r4")]
    s.append(short(
        "Every task that will exist on the chip, ours and the SDK's, with core, priority, stack, trigger and status. In Stage 4 the application adds four tasks; five more exist in the architecture for WP8 and WP9. "
        "The table in app_config.h (cfgTaskSpec) is the single source of truth for names, cores, priorities and stack sizes – code never hard-codes them."))
    s.append(H2("4.1 Application tasks"))
    s.append(table(["Task", "Core / prio", "Stack", "Wakes on", "Owns", "Status"], [
        ["Safety", "0 / 6", "3 KiB", "notification from acquisition, or 100 ms timeout", "relay GPIO15, alarm latches, SafetyStatus", tag("plan")],
        ["AcqNormal", "1 / 4", "4 KiB", "notification timeout = next job deadline", "all sensor buses and drivers, the snapshot writer", tag("plan")],
        ["SerialLogger", "1 / 1", "2 KiB", "log queue item (blocks on receive)", "UART0 output", tag("plan")],
        ["Supervisor", "0 / 1", "3 KiB", "1 s timeout", "health checks, degraded status", tag("plan")],
        ["PowerCoordinator", "0 / 5", "4 KiB", "power-sense ISR, transition ACKs", "power mode, acquisition lease", tag("later") + " WP9"],
        ["AcqLowPower", "0 / 4", "4 KiB", "lease grant", "3.3 V sensors in low power", tag("later") + " WP9"],
        ["WifiMqtt", "0 / 2", "6 KiB", "connection events, backoff", "Wi-Fi + MQTT client", tag("later") + " WP8"],
        ["BleTransport", "0 / 2", "6 KiB", "RX queue, telemetry deadlines", "GATT sends", tag("later") + " WP8"],
        ["Configuration", "0 / 2", "4 KiB", "command queue", "NVS, registry", tag("later") + " WP8"],
    ], [0.15, 0.1, 0.07, 0.27, 0.29, 0.12]))
    s.append(H2("4.2 Framework and SDK tasks that share the cores"))
    s.append(table(["Task", "Core / prio", "Role", "Effect on us"], [
        ["loopTask", "1 / 1", "Runs setup() then loop(). Our loop() blocks forever.", "8 KiB stack wasted but harmless; setup() is where we create our tasks."],
        ["IDLE0, IDLE1", "0 / 0, 1 / 0", "Run when nothing else is Ready; free deleted-task memory.", "Must get CPU time: IDLE0 is watched by the TWDT."],
        ["ipc0, ipc1", "0 / 24, 1 / 24", "Execute functions on a specific core on request.", "Brief; used by the SDK e.g. for interrupt allocation."],
        ["esp_timer", "0 / 22 (verify)", "Dispatches esp_timer callbacks.", "We use esp_timer only as a clock, so it stays idle."],
        ["Tmr Svc", "0 / 1", "FreeRTOS software timers.", "Unused by the application."],
        ["Wi-Fi, lwIP (18), NimBLE host", "0 / high", "Radio and network stacks.", tag("later") + " Absent until WP8; then they preempt Safety (D14)."],
    ], [0.18, 0.14, 0.34, 0.34]))
    s.append(H2("4.3 Why these priorities"))
    s.append(bullets([
        "<b>Safety is highest (6)</b> because its work is the shortest and the most urgent: when an alarm event arrives it must act within 10 ms (§19). Being highest on core 0 among application tasks means only SDK tasks and ISRs can delay it.",
        "<b>AcqNormal (4) is alone at its level on core 1</b>; the only other core-1 tasks are SerialLogger and loopTask at 1. So printing can never delay a sensor deadline: the logger only runs while acquisition is Blocked.",
        "<b>Supervisor and SerialLogger are lowest (1)</b>: they observe and report, and must never steal time from the tasks they observe. If they starve, the system still works – only reporting is late.",
        "<b>Equal priorities are avoided on a core</b> wherever possible, so no task depends on time-slicing to make progress.",
    ]))
    s.append(H2("4.4 Why the work is split across cores this way"))
    s.append(P("Acquisition is the heaviest and most timing-sensitive application work and needs no radio, so it gets core 1 almost to itself. Safety stays on core 0 in every power mode, so the relay controller never migrates when WP9 moves sensing to core 0 in low power (§8). "
               "The cost is that from WP8 on Safety shares core 0 with the radio stacks; the ≤ 10 ms target is therefore re-tested under radio load, with a documented fallback (section 10.3)."))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s


# ---------------------------------------------------------------- 5
def boot():
    s = [H1("5. Boot sequence", "r5")]
    s.append(short(
        "setup() runs once in loopTask on core 1. It makes the outputs safe first, then creates the static queues, then starts Safety before acquisition so the first alarm always has a receiver. "
        "A failed step is logged and the boot continues; the relay is already de-energised and only Safety could change that."))
    s.append(G.boot())
    s.append(caption("Figure 2 – setup() order after Stage 4. Function names are the existing public APIs in services/ and diagnostics/."))
    s.append(H2("Step details"))
    s.append(table(["Step", "What happens", "Status"], [
        ["1 Safe outputs", "gpioApplySafeDefaults() writes GPIO15 LOW <i>before</i> making it an output, so the pin never glitches high; then GPIO17 (HC-SR04 TRIG) LOW. The 10 kΩ gate pull-down already holds the relay off from reset until this line.", tag("plan") + " fills the empty stub (audit L4)"],
        ["2 Logger", "logInit() creates the static log queue. Until the SerialLogger task exists, logPrintf() prints directly, so early boot messages are never lost.", tag("plan")],
        ["3 Board gate", "boardProfileValidate(): pins assigned, power sense verified. Printed as 'board gate', not 'production ready' (audit D2).", tag("done") + " (wording " + tag("plan") + ")"],
        ["4 Snapshot", "snapshotStoreInit(): static length-1 queue of one Snapshot.", tag("done")],
        ["5 Safety", "safetyInit(): sticky store, detail queue, SafetyStatus mailbox, task on core 0. The relay stays OFF until the first lamp decision, which itself starts OFF and has a dwell.", tag("plan")],
        ["6 Acquisition", "acquisitionInit(): construct all drivers and HALs, create AcqNormal on core 1. The first frame starts immediately; drivers that need warm-up report STARTING / WARMING.", tag("plan")],
        ["7 Supervisor + logger", "Create SerialLogger (core 1) and Supervisor (core 0). Supervisor logs esp_reset_reason() once – the first line to read after an unexpected reset.", tag("plan")],
        ["Return", "loop() blocks with vTaskDelay(portMAX_DELAY). No work is ever done in loop().", tag("done")],
    ], [0.16, 0.6, 0.24]))
    s.append(example("What the console shows after a normal boot (illustrative)",
        m("[I] hydroponics firmware - stage 4") + "<br/>" +
        m("[I] reset reason: POWERON") + "<br/>" +
        m("[I] board gate: power sense yes, relay yes -> pass") + "<br/>" +
        m("[I] safety: started (core 0, prio 6), lamp auto ON, relay OFF") + "<br/>" +
        m("[I] acq: started (core 1, prio 4), 7 drivers") + "<br/>" +
        m("[I] ds18b20 init -> OK, ROM 28-00000098BB42") + "<br/>" +
        m("[I] sup: t=5 s | airT 23.1 RH 48 | waterT 21.4 | light 63 mV | pH UNCAL | TDS 466 | lamp OFF")))
    s.append(CondPageBreak(PAGE_H * 0.4))
    return s
