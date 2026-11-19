# Vector diagrams for the RTOS design document. Every function returns a Drawing CW wide.
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "architecture"))
from kit import *  # noqa: E402,F401

DONE = NEW3       # implemented
DONE_L = NEW3_L
PLAN = BLUE       # Stage 4 (WP7 + WP10)
PLAN_L = BLUE_L
LATER = GREY      # WP8 / WP9
LATER_L = GREY_L


def legend(d, x, y, items=(("Implemented", DONE_L, DONE), ("Stage 4 (planned)", PLAN_L, PLAN),
                               ("Later (WP8 / WP9)", LATER_L, LATER))):
    for name, fill, edge in items:
        d.add(Rect(x, y, 10, 8, fillColor=fill, strokeColor=edge, strokeWidth=0.8))
        text(d, x + 14, y + 1, name, 6.8, "UI", GREY)
        x += 18 + stringWidth(name, "UI", 6.8) + 10


def task_row(d, x, y, w, name, sub, kind, h=22):
    fill, edge = {"done": (DONE_L, DONE), "plan": (PLAN_L, PLAN), "later": (LATER_L, LATER),
                  "sdk": (colors.white, GREY)}[kind]
    r = Rect(x, y, w, h, fillColor=fill, strokeColor=edge, strokeWidth=0.6, rx=2, ry=2)
    if kind == "later":
        r.strokeDashArray = [2, 2]
    d.add(r)
    d.add(Rect(x, y, 3, h, fillColor=edge, strokeColor=None))
    text(d, x + 8, y + h - 9, name, 7.3, "UIB", NAVY if kind != "later" else GREY)
    text(d, x + 8, y + 4, sub, 6.4, "UI", GREY)


# ------------------------------------------------------------------ Figure: tasks per core
def cores():
    W, H = CW, 368
    d = Drawing(W, H)
    colw = (W - 16) / 2
    for i, (title, sub) in enumerate([("CORE 0 (PRO_CPU)", "Safety, supervision, radios later"),
                                      ("CORE 1 (APP_CPU)", "acquisition and printing")]):
        x = i * (colw + 16)
        d.add(Rect(x, 14, colw, H - 30, fillColor=colors.white, strokeColor=NAVY if i == 0 else TEAL,
                   strokeWidth=0.9, rx=4, ry=4, strokeDashArray=[3, 2]))
        text(d, x + colw / 2, H - 30, title, 8.8, "UIB", NAVY if i == 0 else TEAL, "middle")
        text(d, x + colw / 2, H - 40, sub, 6.8, "UI", GREY, "middle")
    c0 = [("esp_timer (22)", "SDK: runs esp_timer callbacks; priority from IDF 4.4, verify by trace", "sdk"),
          ("ipc0 (24)", "SDK: cross-core calls (esp_ipc); 1 KiB stack", "sdk"),
          ("Wi-Fi / lwIP 18 / NimBLE host", "SDK radio tasks - not created until WP8", "later"),
          ("Safety (6)", "sole relay writer; wakes on event or every 100 ms", "plan"),
          ("PowerCoordinator (5)", "power mode + acquisition lease (WP9)", "later"),
          ("AcqLowPower (4)", "second worker, blocked until WP9 grant", "later"),
          ("WifiMqtt / BleTransport / Configuration (2)", "WP8 transports and NVS owner", "later"),
          ("Supervisor (1)", "1 s health pass: progress, stacks, heap, counters", "plan"),
          ("Tmr Svc (1)", "SDK: FreeRTOS software-timer daemon (unused by app)", "sdk"),
          ("IDLE0 (0)", "watched by the task watchdog (CHECK_IDLE_TASK_CPU0=y)", "sdk")]
    c1 = [("ipc1 (24)", "SDK: cross-core calls; 1 KiB stack", "sdk"),
          ("AcqNormal (4)", "only owner of every sensor bus (D15: single worker)", "plan"),
          ("SerialLogger (1)", "drains the 32-entry log queue to UART0", "plan"),
          ("loopTask (1)", "Arduino loop(): blocks forever (portMAX_DELAY)", "done"),
          ("IDLE1 (0)", "NOT watched by the task watchdog (CPU1 check off)", "sdk")]
    y = H - 72
    for n, s, k in c0:
        task_row(d, 7, y, colw - 14, n, s, k)
        y -= 26
    y = H - 72
    for n, s, k in c1:
        task_row(d, colw + 16 + 7, y, colw - 14, n, s, k)
        y -= 26
    bx = colw + 16 + 7
    box(d, bx, 80, colw - 14, 94, "ISRs (any core that installed them)",
        ["HC-SR04 ECHO edge: stores timestamps only (implemented; core 1, where the shared GPIO ISR service is installed)",
         "Power-sense GPIO8 edge: epoch++ and notify (WP9; core decided in WP9)",
         "Interrupt watchdog: 300 ms, both CPUs"], fill=AMBER_L, edge=AMBER, size=6.6, title_size=7.6)
    text(d, bx, 64, "Number in brackets = FreeRTOS priority (higher wins).", 6.6, "UII", GREY)
    text(d, bx, 54, "Dashed grey = exists only in a later work package.", 6.6, "UII", GREY)
    legend(d, 0, 1, (("Implemented", DONE_L, DONE), ("Stage 4 (planned)", PLAN_L, PLAN),
                     ("Later (WP8 / WP9)", LATER_L, LATER), ("SDK / framework", colors.white, GREY)))
    return d


# ------------------------------------------------------------------ Figure: boot sequence
def boot():
    W, H = CW, 300
    d = Drawing(W, H)
    steps = [("ROM + 2nd-stage bootloader", "flash, PSRAM init, app_main()", "sdk"),
             ("Arduino app_main", "creates loopTask on core 1, which calls setup()", "sdk"),
             ("1  gpioApplySafeDefaults()", "relay GPIO15 LOW then OUTPUT; TRIG GPIO17 LOW", "plan"),
             ("2  logInit()", "static log queue; direct printf until the logger task runs", "plan"),
             ("3  boardProfileValidate()", "board gate: pins assigned, sense verified", "done"),
             ("4  snapshotStoreInit()", "static length-1 queue", "done"),
             ("5  safetyInit()", "static task, core 0 prio 6; relay stays OFF until first decision", "plan"),
             ("6  acquisitionInit()", "static task, core 1 prio 4; constructs drivers, first frame", "plan"),
             ("7  supervisorInit() + logger task", "core 0 prio 1 / core 1 prio 1; logs reset reason", "plan"),
             ("setup() returns", "loop() blocks forever; tasks run on their own", "done")]
    bw = 250
    y = H - 28
    for i, (t, s, k) in enumerate(steps):
        task_row(d, 0, y, bw, t, s, k, h=24)
        if i < len(steps) - 1:
            arrow(d, 20, y, 20, y - 5, GREY, head=3.5, width=0.8)
        y -= 29
    x2 = bw + 22
    box(d, x2, H - 124, W - x2, 96, "Failure rule",
        ["Any init that returns false is logged at ERROR and boot continues.",
         "The relay was already driven LOW in step 1, and only Safety may change it.",
         "If Safety itself failed, nothing ever drives the relay: the lamp stays OFF."],
        fill=RED_L, edge=RED, size=6.8)
    box(d, x2, H - 232, W - x2, 92, "Why this order",
        ["Safe outputs first, before anything that can crash or wait.",
         "Safety exists before acquisition, so the first alarm event has a receiver.",
         "Readers (Safety) tolerate an empty snapshot: snapshotPeek() returns false."],
        fill=TEAL_L, edge=TEAL, size=6.8)
    legend(d, x2, 4)
    return d


# ------------------------------------------------------------------ Figure: one-second frame (planned)
def frame():
    W, H = CW, 262
    d = Drawing(W, H)
    lx, rx = 96, W - 6
    sc = (rx - lx) / 1000.0
    X = lambda t: lx + t * sc
    top = H - 22
    for t in range(0, 1001, 100):
        d.add(Line(X(t), 40, X(t), top + 4, strokeColor=RULE, strokeWidth=0.5 if t % 250 else 0.9))
        text(d, X(t), top + 8, str(t), 6.6, "UI", GREY, "middle")
    rows = ["BME280 (fast lane)", "DS18B20", "SGP30", "HC-SR04", "TDS window", "pH window",
            "Light window", "Snapshot publish", "Worker CPU busy"]
    ys = {}
    for i, name in enumerate(rows):
        y = top - 16 - i * 22
        ys[name] = y
        text(d, 0, y + 2, name, 7.3, "UIB", NAVY)

    def bar(r, t0, dt, col, minw=2.2):
        d.add(Rect(X(t0), ys[r], max(dt * sc, minw), 11, fillColor=col, strokeColor=None))

    def taps(r, t0, n, sp, col=AMBER):
        for k in range(n):
            bar(r, t0 + k * sp, 1, col, 1.6)
        d.add(Rect(X(t0), ys[r] + 4.5, (n - 1) * sp * sc, 2, fillColor=AMBER_L, strokeColor=None))

    for t in (0, 250, 500, 750):
        bar("BME280 (fast lane)", t, 11, TEAL)
    text(d, X(772), ys["BME280 (fast lane)"] + 2.5, "11 ms each (measured)", 6.4)
    bar("DS18B20", 11, 7, NAVY)
    d.add(Rect(X(18), ys["DS18B20"], 743 * sc, 11, fillColor=TEAL_M, strokeColor=None))
    bar("DS18B20", 761, 12, NAVY)
    text(d, X(24), ys["DS18B20"] + 2.5, "start (~7 ms blocking); the probe converts 750 ms; read (~12 ms) after the 750 ms BME280 slot", 6.3, "UI", NAVY)
    bar("SGP30", 18, 1.5, NAVY)
    d.add(Rect(X(19.5), ys["SGP30"], 12 * sc, 11, fillColor=TEAL_M, strokeColor=None))
    bar("SGP30", 32, 1.5, NAVY)
    text(d, X(40), ys["SGP30"] + 2.5, "measure_iaq; the chip converts; read 6 B + CRC (13 ms measured)", 6.4)
    d.add(Rect(X(20), ys["HC-SR04"], 30 * sc, 11, fillColor=AMBER_L, strokeColor=AMBER, strokeWidth=0.5))
    text(d, X(54), ys["HC-SR04"] + 2.5, "after the 1-Wire start; ISR times the echo (<= 30 ms timeout)", 6.4)
    taps("TDS window", 50, 15, 40)
    text(d, X(620), ys["TDS window"] + 2.5, "15 taps x 40 ms = 560 ms", 6.4)
    taps("pH window", 52, 10, 30)
    text(d, X(336), ys["pH window"] + 2.5, "10 taps x 30 ms = 270 ms", 6.4)
    taps("Light window", 54, 10, 11)
    text(d, X(171), ys["Light window"] + 2.5, "10 taps x 11 ms = 99 ms", 6.4)
    bar("Snapshot publish", 0, 2, NAVY)
    for t in (11, 33, 50, 153, 322, 610, 773):
        bar("Snapshot publish", t, 1.5, NAVY, 1.4)
    text(d, X(800), ys["Snapshot publish"] + 2.5, "per result + boundary", 6.4)
    for t0, dt in [(0, 1), (10, 1), (11, 7), (18, 1.5), (20, 1), (32, 1.5), (250, 1), (260, 1), (500, 1), (510, 1), (750, 1), (760, 1), (761, 12)]:
        bar("Worker CPU busy", t0, dt, NAVY)
    for t in range(50, 610, 10):
        bar("Worker CPU busy", t, 0.6, NAVY, 0.8)
    text(d, X(800), ys["Worker CPU busy"] + 2.5, "otherwise Blocked", 6.4)
    d.add(Rect(X(0), 20, 150 * sc, 8, fillColor=AMBER_L, strokeColor=AMBER, strokeWidth=0.5))
    text(d, X(156), 21, "Rev 3 budget: 150 ms of worker time reserved per frame; the real use is far lower (taps are microseconds)", 6.4, "UI", AMBER)
    text(d, W / 2, 5, "Planned interleave (Stage 4), drawn from driver constants and measured bring-up times - not yet a trace.", 6.8, "UII", GREY, "middle")
    return d


# ------------------------------------------------------------------ Figure: DriverRun state machine
def driverrun():
    W, H = CW, 205
    d = Drawing(W, H)
    bw, bh = 104, 44
    pos = {"UNINIT": (0, 140), "IDLE": (150, 140), "PENDING": (300, 140), "RESULT": (300, 30),
           "BACKOFF": (0, 30)}
    sub = {"UNINIT": ["driver constructed,", "init() not yet OK"], "IDLE": ["initialised,", "waits for its due time"],
           "PENDING": ["start() accepted;", "poll() at nextPollMs"], "RESULT": ["readResult(), stamp,", "store, rules, publish"],
           "BACKOFF": ["failed init/poll;", "retry at a later deadline"]}
    for k, (x, y) in pos.items():
        fill, edge = (RED_L, RED) if k == "BACKOFF" else (PLAN_L, PLAN)
        box(d, x, y, bw, bh, k, sub[k], fill=fill, edge=edge, size=6.6, title_size=8)
    arrow(d, 104, 168, 150, 168, PLAN, label="init OK", size=6.4)
    arrow(d, 254, 168, 300, 168, PLAN, label="due: start", size=6.4)
    path_arrow(d, [(404, 162), (420, 162), (420, 192), (380, 192), (380, 184)], PLAN)
    label(d, 424, 176, "PENDING:", PLAN, 6.4, "start")
    label(d, 424, 168, "poll again", PLAN, 6.4, "start")
    arrow(d, 380, 140, 380, 74, DONE)
    label(d, 384, 104, "READY", DONE, 6.4, "start")
    path_arrow(d, [(300, 52), (230, 52), (230, 140)], DONE)
    label(d, 234, 60, "done; wait for next due", DONE, 6.4, "start")
    path_arrow(d, [(320, 140), (320, 100), (85, 100), (85, 74)], RED)
    label(d, 150, 104, "FAULT / TIMEOUT / NOT_PRESENT", RED, 6.4)
    arrow(d, 20, 140, 20, 74, RED)
    label(d, 24, 120, "init fails", RED, 6.4, "start")
    arrow(d, 55, 74, 55, 140, RED)
    label(d, 59, 84, "retry at deadline", RED, 6.2, "start")
    box(d, 412, 18, W - 412, 104, "Frame guard", ["Still PENDING at its frame deadline: cancel(), TIMEOUT + kQualityLate; not retried in that frame."],
        fill=AMBER_L, edge=AMBER, size=6.4, title_size=7.6)
    text(d, 0, 4, "One DriverRun per driver. The same stepping is proven in test/bringup/water_main.c (beginRun / stepRun / cancelRun).", 6.6, "UII", GREY)
    return d


# ------------------------------------------------------------------ Figure: acquisition worker loop
def acq_loop():
    W, H = CW, 330
    d = Drawing(W, H)
    bw = 236
    x = 0
    steps = [("wait: ulTaskNotifyTake(timeout = next due - now)", "woken by its deadline, or early by WP9 / a refresh request", NAVY),
             ("now = monoNowMs()", "one clock for every decision in this pass", NAVY),
             ("frame boundary due?", "yes: record release jitter, publish snapshot, start frame jobs", AMBER),
             ("BME280 slot due?", "the fast lane always goes first", TEAL),
             ("step every due DriverRun once", "start / poll; defer a step that would cross the next BME280 slot", PLAN),
             ("for each READY: readResult()", "stamp powerEpoch, configRevision, sequence", PLAN),
             ("store into working Snapshot", "readings[metric] = copy; cross-sensor inputs updated", PLAN),
             ("evaluateRule() for that metric", "TRIP / CLEAR -> safetySubmitEvent() (zero wait)", RED),
             ("snapshotPublish()", "xQueueOverwrite: readers see it at once", PLAN),
             ("progressMark + esp_task_wdt_reset", "only after meaningful work", DONE),
             ("next due = min(all job deadlines)", "absolute times; missed slots skipped, not replayed", NAVY)]
    y = H - 30
    for i, (t, s, col) in enumerate(steps):
        d.add(Rect(x, y, bw, 24, fillColor=colors.white, strokeColor=col, strokeWidth=0.7, rx=2, ry=2))
        d.add(Rect(x, y, 3, 24, fillColor=col, strokeColor=None))
        text(d, x + 8, y + 14.5, t, 7.2, "UIB", NAVY)
        text(d, x + 8, y + 4.5, s, 6.3, "UI", GREY)
        if i < len(steps) - 1:
            arrow(d, x + 16, y, x + 16, y - 5, GREY, head=3.5, width=0.8)
        y -= 29
    path_arrow(d, [(bw, y + 29 + 12), (bw + 12, y + 29 + 12), (bw + 12, H - 18), (bw, H - 18)], NAVY)
    label(d, bw + 15, H / 2, "loop", NAVY, 6.6, "start")
    x2 = bw + 40
    box(d, x2, H - 120, W - x2, 92, "Rules the loop never breaks",
        ["No vTaskDelay(1000): a one-second sleep would hide the 250 ms BME280 slots.",
         "No busy wait: between taps the task is Blocked and uses no CPU.",
         "No network, NVS or printf calls: log through the queue only."], fill=TEAL_L, edge=TEAL, size=6.6)
    box(d, x2, H - 238, W - x2, 106, "Why a notify-wait, not vTaskDelayUntil",
        ["vTaskDelayUntil sleeps a fixed period and cannot be woken early.",
         "ulTaskNotifyTake with a timeout sleeps until the next deadline, but WP9's power coordinator (or a refresh command) can wake it at once with xTaskNotifyGive."],
        fill=AMBER_L, edge=AMBER, size=6.6)
    box(d, x2, 12, W - x2, 100, "Bounded pass",
        ["Each step is <= 5 ms CPU (Rev 3 rule); measured: BME280 11 ms elapsed incl. 9.3 ms conversion wait done as PENDING polls.",
         "One pass therefore finishes in a few ms and the task blocks again."], fill=PLAN_L, edge=PLAN, size=6.6)
    return d


# ------------------------------------------------------------------ Figure: Safety wake loop
def safety_loop():
    W, H = CW, 236
    d = Drawing(W, H)
    bw = 250
    steps = [("Block: ulTaskNotifyTake(timeout 100 ms)", "event notify from acquisition, or the health-check deadline", NAVY),
             ("1  Power validity", "until WP9: always valid; later: invalidate 5 V metrics by epoch", LATER),
             ("2  Drain sticky alarms + detail queue", "latch, worst value, count; apply interlock action now", RED),
             ("3  Freshness, every wake", "snapshotPeek(); stale or faulty light -> lamp OFF; stale-data alarms", AMBER),
             ("4  Commands", "none until WP8 (remote never overrides a local interlock)", LATER),
             ("5  Outputs", "relay = interlock ? OFF : lamp decision; write GPIO only on change", PLAN),
             ("Publish SafetyStatus + progressMark + wdt reset", "length-1 mailbox for BLE / MQTT later", DONE)]
    y = H - 30
    for i, (t, s, col) in enumerate(steps):
        d.add(Rect(0, y, bw, 25, fillColor=colors.white, strokeColor=col, strokeWidth=0.7, rx=2, ry=2))
        d.add(Rect(0, y, 3, 25, fillColor=col, strokeColor=None))
        text(d, 8, y + 15, t, 7.2, "UIB", NAVY)
        text(d, 8, y + 4.5, s, 6.3, "UI", GREY)
        if i < len(steps) - 1:
            arrow(d, 16, y, 16, y - 5, GREY, head=3.5, width=0.8)
        y -= 30
    path_arrow(d, [(bw, y + 30 + 12), (bw + 12, y + 30 + 12), (bw + 12, H - 18), (bw, H - 18)], NAVY)
    x2 = bw + 32
    box(d, x2, H - 112, W - x2, 84, "Why it wakes every 100 ms",
        ["An acquisition stall produces NO events. Only a timed wake can notice that the light reading has stopped ageing forward and switch the lamp OFF. (Fixes audit B1.)"],
        fill=AMBER_L, edge=AMBER, size=6.6)
    box(d, x2, 6, W - x2, 112, "Static memory (3 KiB stack)",
        ["The 584-byte Snapshot copy and the drained event buffer are static variables, not locals.",
         "Safety never blocks on anything except its own notification: no mutex, no queue receive with a timeout, no NVS, no network."],
        fill=PLAN_L, edge=PLAN, size=6.6)
    return d


# ------------------------------------------------------------------ Figure: Stage 4 channels
def channels():
    W, H = CW, 250
    d = Drawing(W, H)
    bw, bh = 120, 50
    gap = (W - 3 * bw) / 2
    X = [0, bw + gap, 2 * (bw + gap)]
    Y = [185, 100, 15]
    box(d, X[0], Y[0], bw, bh, "AcqNormal (core 1)", ["single bus owner", "writer of snapshot"], fill=PLAN_L, edge=PLAN, size=6.6)
    box(d, X[1], Y[0], bw, bh, "Snapshot store", ["length-1 queue, 584 B", "overwrite / peek"], fill=DONE_L, edge=DONE, size=6.6)
    box(d, X[2], Y[0], bw, bh, "BLE / MQTT (WP8)", ["peek, zero wait", "never consume"], fill=LATER_L, edge=LATER, size=6.6)
    box(d, X[0], Y[1], bw, bh, "Sticky alarm store", ["1 slot per rule, portMUX", "+ 16-record detail queue"], fill=AMBER_L, edge=AMBER, size=6.6)
    box(d, X[1], Y[1], bw, bh, "Safety (core 0)", ["sole relay writer", "100 ms health wake"], fill=PLAN_L, edge=PLAN, size=6.6)
    box(d, X[2], Y[1], bw, bh, "SafetyStatus", ["length-1 mailbox", "relay + alarm summary"], fill=PLAN_L, edge=PLAN, size=6.6)
    box(d, X[0], Y[2], bw, bh, "All tasks", ["logPrintf(): format,", "zero-wait send"], fill=GREY_L, edge=GREY, size=6.6)
    box(d, X[1], Y[2], bw, bh, "Log queue -> SerialLogger", ["32 x fixed records", "drop + count when full"], fill=PLAN_L, edge=PLAN, size=6.6)
    box(d, X[2], Y[2], bw, bh, "Supervisor (core 0)", ["reads progress, stacks,", "heap, all counters"], fill=PLAN_L, edge=PLAN, size=6.6)
    m = bh / 2
    g = gap / 2
    arrow(d, X[0] + bw, Y[0] + m, X[1], Y[0] + m, TEAL)
    label(d, X[0] + bw + g, Y[0] + m + 5, "xQueueOverwrite", TEAL, 6.2)
    arrow(d, X[1] + bw, Y[0] + m, X[2], Y[0] + m, LATER, dash=[2, 2])
    label(d, X[1] + bw + g, Y[0] + m + 5, "xQueuePeek(0)", LATER, 6.2)
    arrow(d, X[0] + 30, Y[0], X[0] + 30, Y[1] + bh, AMBER)
    label(d, X[0] + 34, Y[1] + bh + 12, "rule TRIP / CLEAR", AMBER, 6.2, "start")
    arrow(d, X[0] + bw, Y[1] + m, X[1], Y[1] + m, AMBER)
    label(d, X[0] + bw + g, Y[1] + m + 5, "xTaskNotifyGive", AMBER, 6.2)
    path_arrow(d, [(X[1] + bw / 2, Y[0]), (X[1] + bw / 2, Y[1] + bh)], TEAL)
    label(d, X[1] + bw / 2 + 4, Y[1] + bh + 12, "peek every wake", TEAL, 6.2, "start")
    arrow(d, X[1] + bw, Y[1] + m, X[2], Y[1] + m, PLAN)
    label(d, X[1] + bw + g, Y[1] + m + 5, "overwrite", PLAN, 6.2)
    arrow(d, X[0] + bw, Y[2] + m, X[1], Y[2] + m, GREY)
    label(d, X[0] + bw + g, Y[2] + m + 5, "xQueueSend(0)", GREY, 6.2)
    arrow(d, X[1] + bw, Y[2] + m, X[2], Y[2] + m, GREY, dash=[2, 2])
    label(d, X[1] + bw + g, Y[2] + m + 5, "drop counter", GREY, 6.2)
    path_arrow(d, [(X[1] + 20, Y[1]), (X[1] + 20, Y[1] - 18), (X[1] - 14, Y[1] - 18)], RED)
    label(d, X[1] + 24, Y[1] - 14, "GPIO15", RED, 6.2, "start")
    return d


# ------------------------------------------------------------------ Figure: per-core timeline for an alarm
def core_timeline():
    W, H = CW, 176
    d = Drawing(W, H)
    lx, rx = 90, W - 6
    t0, t1 = 245.0, 275.0
    sc = (rx - lx) / (t1 - t0)
    X = lambda t: lx + (t - t0) * sc
    top = H - 22
    for t in range(245, 276, 5):
        d.add(Line(X(t), 30, X(t), top + 4, strokeColor=RULE, strokeWidth=0.5))
        text(d, X(t), top + 8, "%d ms" % t, 6.4, "UI", GREY, "middle")
    lanes = [("Core 1: AcqNormal", top - 18), ("Core 1: idle", top - 40),
             ("Core 0: Safety", top - 72), ("Core 0: Supervisor", top - 94), ("Core 0: idle", top - 116)]
    for n, y in lanes:
        text(d, 0, y + 2.5, n, 7.1, "UIB", NAVY)

    def seg(lane, a, b, col):
        y = dict(lanes)[lane]
        d.add(Rect(X(a), y, (b - a) * sc, 11, fillColor=col, strokeColor=None))

    seg("Core 1: idle", 245, 250, GREY_L)
    seg("Core 1: AcqNormal", 250, 250.6, PLAN)
    seg("Core 1: idle", 250.6, 260.4, GREY_L)
    for t in (253, 256, 259):
        seg("Core 1: AcqNormal", t, t + 0.3, PLAN)
    seg("Core 1: AcqNormal", 261, 262.3, PLAN)
    seg("Core 1: idle", 262.3, 275, GREY_L)
    seg("Core 0: Supervisor", 245, 249, LATER_L)
    seg("Core 0: idle", 249, 262.35, GREY_L)
    seg("Core 0: Safety", 262.35, 263.0, RED)
    seg("Core 0: idle", 263.0, 275, GREY_L)
    text(d, X(250) + 2, dict(lanes)["Core 1: AcqNormal"] + 14, "start forced conversion", 6.2, "UI", PLAN)
    text(d, X(253) - 2, dict(lanes)["Core 1: AcqNormal"] - 8, "PENDING polls (blocked between)", 6.2, "UI", PLAN)
    text(d, X(262.3) + 3, dict(lanes)["Core 1: AcqNormal"] + 2.5, "read 50 C, rule TRIP, submit + notify", 6.2, "UIB", RED)
    arrow(d, X(262.3), dict(lanes)["Core 1: AcqNormal"], X(262.35), dict(lanes)["Core 0: Safety"] + 11, RED, width=0.8, head=4)
    text(d, X(263) + 3, dict(lanes)["Core 0: Safety"] + 2.5, "drain, latch, relay OFF, status", 6.2, "UIB", RED)
    text(d, X(263) + 3, dict(lanes)["Core 0: Safety"] - 7, "target <= 10 ms; expected < 1 ms", 6.2, "UI", RED)
    text(d, W / 2, 6, "Illustration, not a trace: the event crosses cores through a task notification; Safety (priority 6) preempts anything lower on core 0.", 6.6, "UII", GREY, "middle")
    return d


# ------------------------------------------------------------------ Figure: lamp hysteresis and dwell
def lamp():
    W, H = CW, 196
    d = Drawing(W, H)
    lx, rx = 40, W - 6
    T = 300.0
    sc = (rx - lx) / T
    X = lambda t: lx + t * sc
    yb, yh = 70, 110
    V = lambda mv: yb + mv / 200.0 * yh
    d.add(Line(lx, yb, rx, yb, strokeColor=GREY, strokeWidth=0.6))
    d.add(Line(lx, yb, lx, yb + yh, strokeColor=GREY, strokeWidth=0.6))
    for mv in (0, 40, 120, 200):
        text(d, lx - 4, V(mv) - 2, str(mv), 6.4, "UI", GREY, "end")
    text(d, 2, yb + yh + 6, "light mV", 6.6, "UIB", NAVY)
    for mv, col, lab in ((40, TEAL, "ON below 40 mV"), (120, AMBER, "OFF above 120 mV")):
        ln = Line(lx, V(mv), rx, V(mv), strokeColor=col, strokeWidth=0.7)
        ln.strokeDashArray = [3, 2]
        d.add(ln)
        text(d, rx - 2, V(mv) + 3, lab, 6.4, "UI", col, "end")
    pts = [(0, 150), (5, 150), (14, 20), (130, 15), (140, 80), (165, 95), (175, 170), (230, 175), (240, 60), (300, 70)]
    for (a, va), (b, vb) in zip(pts[:-1], pts[1:]):
        d.add(Line(X(a), V(va), X(b), V(vb), strokeColor=NAVY, strokeWidth=1.2))
    yl = 30
    text(d, 2, yl + 3, "lamp", 6.6, "UIB", NAVY)
    segs = [(0, 30, False), (30, 205, True), (205, 300, False)]
    for a, b, on in segs:
        d.add(Rect(X(a), yl, (b - a) * sc, 12, fillColor=TEAL if on else GREY_L, strokeColor=RULE, strokeWidth=0.4))
        text(d, X((a + b) / 2), yl + 3.5, "ON" if on else "OFF", 6.6, "UIB", colors.white if on else GREY, "middle")
    for a, b, lab in [(0, 30, "boot dwell"), (175, 205, "dwell 30 s")]:
        d.add(Rect(X(a), yl + 14, (b - a) * sc, 4, fillColor=AMBER, strokeColor=None))
        text(d, X((a + b) / 2), yl + 21, lab, 6.2, "UI", AMBER, "middle")
    text(d, X(80), V(80), "between 40 and 120: no change", 6.4, "UII", GREY, "middle")
    text(d, X(70), V(28), "dark from 14 s; ON when the boot dwell ends", 6.4, "UII", GREY, "middle")
    for t in range(0, 301, 60):
        text(d, X(t), yl - 10, "%d s" % t, 6.4, "UI", GREY, "middle")
    text(d, W / 2, 4, "Placeholder thresholds and dwell from test_lamp; final values are measured at the sensor's final position.", 6.6, "UII", GREY, "middle")
    return d


# ------------------------------------------------------------------ Figure: who feeds which watchdog
def watchdogs():
    W, H = CW, 196
    d = Drawing(W, H)
    cw = 130
    xm, xr = 180, W - cw
    box(d, 0, 130, cw, 52, "AcqNormal", ["resets the TWDT for itself", "after a pass with real work"], fill=PLAN_L, edge=PLAN, size=6.6)
    box(d, 0, 56, cw, 52, "Safety", ["resets the TWDT for itself", "after each 100 ms pass"], fill=PLAN_L, edge=PLAN, size=6.6)
    box(d, xm, 112, cw, 70, "Task watchdog (TWDT)", ["5 s timeout", "panic + reset on expiry", "reason logged at next boot"], fill=RED_L, edge=RED, size=6.6)
    box(d, xm, 22, cw, 58, "IDLE0", ["subscribed by the SDK:", "catches starvation of core 0"], fill=GREY_L, edge=GREY, size=6.6)
    box(d, xr, 130, cw, 52, "Supervisor", ["checks stamps every 1 s,", "warns before the TWDT fires"], fill=PLAN_L, edge=PLAN, size=6.6)
    box(d, xr, 40, cw, 62, "Progress stamps", ["progressMark(task)", "64-bit, portMUX-protected"], fill=PLAN_L, edge=PLAN, size=6.6)
    arrow(d, cw, 156, xm, 156, RED, label="reset own", size=6.2)
    path_arrow(d, [(cw, 82), (xm - 18, 82), (xm - 18, 130), (xm, 130)], RED)
    label(d, xm - 22, 92, "reset own", RED, 6.2, "end")
    arrow(d, xm + cw / 2, 80, xm + cw / 2, 112, GREY)
    path_arrow(d, [(30, 56), (30, 8), (xr + cw / 2, 8), (xr + cw / 2, 40)], PLAN, dash=[2, 2])
    path_arrow(d, [(cw - 20, 130), (cw - 20, 120), (cw + 22, 120), (cw + 22, 14), (xr + cw / 2 - 20, 14), (xr + cw / 2 - 20, 40)], PLAN, dash=[2, 2])
    label(d, (xm + cw + xr) / 2 + 30, 18, "progressMark()", PLAN, 6.2)
    arrow(d, xr + cw / 2, 102, xr + cw / 2, 130, PLAN)
    label(d, xr + cw / 2 + 4, 114, "read", PLAN, 6.2, "start")
    label(d, xr - 6, 186, "Supervisor never resets the TWDT for another task", RED, 6.6, "end", "UIB")
    return d
