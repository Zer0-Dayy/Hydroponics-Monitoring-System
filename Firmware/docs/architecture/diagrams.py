# Vector diagrams for the architecture baseline. Every function returns a reportlab Drawing CW wide.
from kit import *


def glance():
    W, H = CW, 360
    d = Drawing(W, H)
    c1x, c1w = 0, 150
    chx, chw = 174, 148
    c0x, c0w = 346, W - 346
    mid_top, mid_bot = 290, 50
    box(d, 0, 316, chx + chw, 42, "SENSORS",
        ["Retained 3.3 V: BME280, SGP30 (I2C 21/47), DS18B20 (14), light (ADC1 1)",
         "External 5 V: HC-SR04 (17/18), TDS (ADC1 6), pH (ADC1 7)"], fill=GREY_L, edge=GREY, size=7)
    box(d, c0x, 316, c0w, 42, "POWER-PRESENT INPUT", ["GPIO 8 ≈ 3.3 V = normal", "0 V = low power"],
        fill=RED_L, edge=RED, size=7)
    box(d, c0x, 0, c0w, 32, "RELAY OUTPUT", ["GPIO 15, active-high, ext. 5 V coil"], fill=RED_L, edge=RED, size=6.8)
    d.add(Rect(c1x, mid_bot, c1w, mid_top - mid_bot, fillColor=colors.white, strokeColor=TEAL, strokeWidth=0.9,
               rx=4, ry=4, strokeDashArray=[3, 2]))
    text(d, c1x + c1w / 2, mid_top - 13, "CORE 1 (APP_CPU)", 8.6, "UIB", TEAL, "middle")
    text(d, c1x + c1w / 2, mid_top - 23, "sensing + printing", 7, "UI", GREY, "middle")
    box(d, c1x + 8, 160, c1w - 16, 98, "AcquisitionNormal (4)",
        ["Only owner of every sensor bus in normal mode", "1 s frame + BME280 every 250 ms", "Checks thresholds right after each read"])
    box(d, c1x + 8, 92, c1w - 16, 50, "SerialLogger (1)", ["Prints bounded log records"])
    text(d, c1x + c1w / 2, 68, "Both tasks block in low power;", 6.8, "UII", GREY, "middle")
    text(d, c1x + c1w / 2, 58, "core 1 then only idles", 6.8, "UII", GREY, "middle")
    text(d, chx + chw / 2, mid_top - 13, "SHARED CHANNELS", 8.6, "UIB", AMBER, "middle")
    text(d, chx + chw / 2, mid_top - 23, "fixed-size copies, never pointers", 7, "UI", GREY, "middle")
    ch = [("Latest snapshot", ["1 record, overwritten", "BLE / MQTT / Safety peek, never wait"], 214),
          ("Sticky alarm + detail queue", ["sticky bit per source + 16 records", "wakes Safety immediately"], 158),
          ("Command ingress", ["8 records, reject when busy", "BLE / MQTT -> Configuration, Safety"], 102),
          ("Mode / lease control", ["one grant each", "QUIESCE / RELEASED / GRANT"], 52)]
    for t, ls, y in ch:
        box(d, chx, y, chw, 48, t, ls, fill=AMBER_L, edge=AMBER, size=6.8)
    d.add(Rect(c0x, mid_bot, c0w, mid_top - mid_bot, fillColor=colors.white, strokeColor=NAVY, strokeWidth=0.9,
               rx=4, ry=4, strokeDashArray=[3, 2]))
    text(d, c0x + c0w / 2, mid_top - 13, "CORE 0 (PRO_CPU)", 8.6, "UIB", NAVY, "middle")
    text(d, c0x + c0w / 2, mid_top - 23, "safety, power, radios, configuration", 7, "UI", GREY, "middle")
    rows = [("Safety (6)", "sole relay writer, latched alarms"),
            ("PowerCoordinator (5)", "mode + acquisition lease"),
            ("AcquisitionLowPower (4)", "runs only in low power"),
            ("WiFiMqtt (2)", "sole MQTT client"),
            ("BleTransport (2)", "sole GATT sender"),
            ("Configuration (2)", "sole NVS / registry writer"),
            ("Supervisor (1)", "progress, heap, stacks"),
            ("SDK radio tasks", "Wi-Fi, lwIP, NimBLE: higher priority")]
    y = mid_top - 52
    for i, (t, s) in enumerate(rows):
        sdk = i == len(rows) - 1
        d.add(Rect(c0x + 7, y, c0w - 14, 24, fillColor=GREY_L if sdk else BLUE_L, strokeColor=RULE, strokeWidth=0.5, rx=2, ry=2))
        text(d, c0x + 12, y + 14, t, 7.4, "UIB", GREY if sdk else NAVY)
        text(d, c0x + 12, y + 4.5, s, 6.6, "UI", GREY)
        y -= 27
    arrow(d, 20, 316, 20, 260, GREY)
    label(d, 25, 300, "bus I/O (normal mode)", GREY, anchor="start")
    arrow(d, c0x + 20, 316, c0x + 20, 292, RED)
    label(d, c0x + 25, 300, "ISR: epoch++, wake PowerCoordinator", RED, 6.4, "start")
    arrow(d, c0x + 20, 50, c0x + 20, 33, RED)
    label(d, c0x + 25, 38, "Safety drives", RED, 6.4, "start")
    for yy, col in [(238, TEAL), (182, AMBER)]:
        arrow(d, c1x + c1w - 8, yy, chx, yy, col)
        arrow(d, chx + chw, yy, c0x, yy, col)
    arrow(d, c0x, 126, chx + chw, 126, NAVY)
    arrow(d, c0x, 76, chx + chw, 76, NAVY)
    arrow(d, chx, 76, c1x + c1w, 76, NAVY, dash=[2, 2])
    return d

def layers():
    W, H = CW, 150
    d = Drawing(W, H)
    box(d, 0, 118, W, 28, "src/main.c - bootstrap only: safe outputs, config, static queues, create tasks, then step aside",
        [], fill=GREY_L, edge=GREY, title_size=8, top_bar=False, center=True)
    w = (W - 40) / 3
    items = [("1. Drivers + HAL", ["Hardware transactions (I2C, 1-Wire, ADC, capture, GPIO)", "Bounded, typed results", "No RTOS policy, no network"]),
             ("2. Services + domain", ["Schedule and validate work", "Own resources and policy", "Units, alarms, calibration"]),
             ("3. Transports", ["Copy latest records", "Serialize to MQTT / BLE", "Send commands inward via queues"])]
    for i, (t, ls) in enumerate(items):
        box(d, i * (w + 20), 30, w, 78, t, ls)
        if i < 2:
            arrow(d, i * (w + 20) + w + 1, 78, (i + 1) * (w + 20) - 1, 78, TEAL, label="data")
            arrow(d, (i + 1) * (w + 20) - 1, 50, i * (w + 20) + w + 1, 50, AMBER, label="cmds", label_dy=-9)
    text(d, W / 2, 10, "Sensor data flows right. Commands flow left only through validated queues.", 7.6, "UII", GREY, "middle")
    return d


def modes():
    W, H = CW, 205
    d = Drawing(W, H)
    pw = (W - 16) / 2
    for i, (mode, c1, c0) in enumerate([
        ("NORMAL MODE (external power present)",
         [("AcquisitionNormal", True), ("SerialLogger", True)],
         [("Safety", True), ("PowerCoordinator", True), ("AcquisitionLowPower", False), ("WiFiMqtt", True),
          ("BleTransport", True), ("Configuration", True), ("Supervisor", True)]),
        ("LOW-POWER MODE (external power absent)",
         [("AcquisitionNormal", False), ("SerialLogger", False)],
         [("Safety", True), ("PowerCoordinator", True), ("AcquisitionLowPower", True), ("WiFiMqtt", False),
          ("BleTransport", True), ("Configuration", True), ("Supervisor", True)])]):
        x = i * (pw + 16)
        d.add(Rect(x, 0, pw, H, fillColor=GREY_L if i else TEAL_L, strokeColor=RULE, rx=4, ry=4))
        text(d, x + pw / 2, H - 14, mode, 8.4, "UIB", NAVY, "middle")
        cw = (pw - 24) / 2
        for j, (core, lst) in enumerate([("Core 1", c1), ("Core 0", c0)]):
            cx = x + 8 + j * (cw + 8)
            text(d, cx + cw / 2, H - 30, core, 8, "UIB", TEAL if j == 0 else NAVY, "middle")
            y = H - 52
            for name, active in lst:
                r = Rect(cx, y, cw, 17, fillColor=colors.white if active else GREY_L,
                         strokeColor=TEAL if active else GREY, strokeWidth=0.8, rx=2, ry=2)
                if not active:
                    r.strokeDashArray = [2, 2]
                d.add(r)
                text(d, cx + cw / 2, y + 5.5, name + ("" if active else " (blocked)"), 6.9, "UI" if active else "UII",
                     TEXT if active else GREY, "middle")
                y -= 21
        if i == 1:
            text(d, x + 8 + cw / 2, 50, "Idle task only:", 7, "UII", GREY, "middle")
            text(d, x + 8 + cw / 2, 40, "core 1 waits in WAITI", 7, "UII", GREY, "middle")
            text(d, x + 8 + cw / 2, 30, "(not powered off)", 7, "UII", GREY, "middle")
        else:
            text(d, x + 8 + cw / 2, 40, "Wi-Fi + MQTT + BLE on", 7, "UII", GREY, "middle")
            text(d, x + 8 + cw / 2, 30, "all qualified sensors", 7, "UII", GREY, "middle")
    return d


def timing():
    W, H = CW, 201
    d = Drawing(W, H)
    lx, rx = 92, W - 12
    sc = (rx - lx) / 1000.0
    X = lambda t: lx + t * sc
    top = H - 24
    for t in range(0, 1001, 250):
        d.add(Line(X(t), 32, X(t), top + 8, strokeColor=RULE, strokeWidth=0.6))
        text(d, X(t), top + 12, str(t), 7.2, "UI", GREY, "middle")
    rows = [("BME280 (fast lane)", top - 14), ("DS18B20", top - 40), ("SGP30", top - 66), ("HC-SR04", top - 92),
            ("Analog x3 (pH, TDS, light)", top - 118), ("Snapshot / policy", top - 144)]
    for name, y in rows:
        for i, tl in enumerate(wrap(name, "UIB", 7.4, lx - 8)):
            text(d, 0, y + 3 - i * 9, tl, 7.4, "UIB", NAVY)
    y = rows[0][1]
    for t in (0, 250, 500, 750):
        d.add(Rect(X(t), y, max(12 * sc, 3), 10, fillColor=TEAL, strokeColor=None))
    text(d, X(262) + 4, y + 2, "12 ms each, every 250 ms", 6.8)
    y = rows[1][1]
    d.add(Rect(X(12), y, 20 * sc, 10, fillColor=NAVY, strokeColor=None))
    d.add(Rect(X(32), y, 750 * sc, 10, fillColor=TEAL_M, strokeColor=None))
    text(d, X(40), y + 2.5, "read previous + request next (20 ms), then conversion runs inside the sensor (up to 750 ms)", 6.8, "UI", NAVY)
    y = rows[2][1]
    d.add(Rect(X(32), y, 5 * sc, 10, fillColor=NAVY, strokeColor=None))
    d.add(Rect(X(37), y, 12 * sc, 10, fillColor=TEAL_M, strokeColor=None))
    d.add(Rect(X(67), y, 10 * sc, 10, fillColor=NAVY, strokeColor=None))
    text(d, X(81), y + 2.5, "start (5 ms), conversion inside the sensor, then read + CRC (10 ms)", 6.8, "UI", NAVY)
    y = rows[3][1]
    d.add(Rect(X(37), y, 30 * sc, 10, fillColor=AMBER, strokeColor=None))
    text(d, X(71), y + 2, "capture with 30 ms timeout", 6.8)
    y = rows[4][1]
    for k in range(3):
        d.add(Rect(X(77 + k * 5), y, 5 * sc - 0.6, 10, fillColor=AMBER, strokeColor=None))
    text(d, X(96), y + 2, "3 x 5 ms CPU slots (filter taps may be spaced across the frame)", 6.8)
    y = rows[5][1]
    d.add(Rect(X(92), y, 10 * sc, 10, fillColor=NAVY, strokeColor=None))
    text(d, X(106), y + 2, "10 ms", 6.8)
    d.add(Rect(X(0), 18, 150 * sc, 8, fillColor=AMBER_L, strokeColor=AMBER, strokeWidth=0.6))
    text(d, X(155), 19, "150 ms reserved per frame (48 + 20 + 15 + 30 + 15 + 10 = 138 ms, rounded up); the rest is idle", 6.8, "UI", AMBER)
    text(d, W / 2, 4, "Illustrative budget, not a measured trace. Time in milliseconds within one 1-second frame.", 7, "UII", GREY, "middle")
    return d


def latency():
    W, H = CW, 70
    d = Drawing(W, H)
    sc = (W - 10) / 310.0
    parts = [(250, "250 ms worst-case wait for the next BME280 sample", TEAL_M, NAVY),
             (5, "", AMBER, None), (12, "", TEAL, None), (10, "", NAVY, None)]
    x = 0
    for v, lab, col, tc in parts:
        d.add(Rect(x, 34, v * sc, 18, fillColor=col, strokeColor=colors.white, strokeWidth=0.5))
        if lab:
            text(d, x + 6, 40, lab, 7.4, "UIB", tc)
        x += v * sc
    d.add(Line(300 * sc, 28, 300 * sc, 58, strokeColor=RED, strokeWidth=1, strokeDashArray=[2, 2]))
    text(d, 300 * sc, 61, "300 ms acceptance target", 7, "UIB", RED, "end")
    text(d, 277 * sc, 22, "277 ms", 7.4, "UIB", NAVY, "middle")
    text(d, 252 * sc, 8, "+5 ms scheduling   +12 ms conversion/read   +10 ms Safety action", 7, "UI", GREY, "end")
    return d


def dataflow():
    W, H = CW, 268
    d = Drawing(W, H)
    bw, bh = 128, 58
    gap = (W - 3 * bw) / 2
    X = [0, bw + gap, 2 * (bw + gap)]
    Y = [200, 110, 20]
    box(d, X[0], Y[0], bw, bh, "Active acquisition worker", ["the one lease holder", "no network calls"])
    box(d, X[1], Y[0], bw, bh, "Latest snapshot", ["1 record, copied,", "overwritten"])
    box(d, X[2], Y[0], bw, bh, "BLE + MQTT transports", ["independent readers", "alarm journal cursors"])
    box(d, X[0], Y[1], bw, bh, "Sticky alarm state", ["sticky bit per source", "+ 16-record detail queue"], fill=AMBER_L, edge=AMBER)
    box(d, X[1], Y[1], bw, bh, "Safety", ["sole actuator writer", "journal (64) + current state"], fill=AMBER_L, edge=AMBER)
    box(d, X[2], Y[1], bw, bh, "Configuration", ["command ingress (8)", "sole NVS / registry writer"], fill=BLUE_L, edge=NAVY)
    box(d, X[0], Y[2], bw, bh, "Power-present ISR", ["epoch++ and invalidate", "then notify"], fill=RED_L, edge=RED)
    box(d, X[1], Y[2], bw, bh, "PowerCoordinator", ["mode mailbox (single writer)", "grants the acquisition lease"], fill=BLUE_L, edge=NAVY)
    m = bh / 2
    g = gap / 2
    arrow(d, X[0] + bw, Y[0] + 40, X[1], Y[0] + 40, TEAL)
    label(d, X[0] + bw + g, Y[0] + 44, "overwrite", TEAL)
    arrow(d, X[1] + bw, Y[0] + 40, X[2], Y[0] + 40, TEAL)
    label(d, X[1] + bw + g, Y[0] + 44, "peek, 0 wait", TEAL)
    arrow(d, X[0] + 40, Y[0], X[0] + 40, Y[1] + bh, AMBER)
    label(d, X[0] + 44, Y[1] + bh + 12, "threshold event", AMBER, anchor="start")
    arrow(d, X[0] + bw, Y[1] + m, X[1], Y[1] + m, AMBER)
    label(d, X[0] + bw + gap * 0.35, Y[1] + m + 6, "notify", AMBER)
    path_arrow(d, [(X[1] + bw, Y[1] + bh - 12), (X[1] + bw + g, Y[1] + bh - 12), (X[1] + bw + g, Y[0] + 14), (X[2], Y[0] + 14)], AMBER)
    label(d, X[1] + bw + g + 3, Y[1] + bh + 4, "journal", AMBER, anchor="start")
    arrow(d, X[2] + bw / 2, Y[0], X[2] + bw / 2, Y[1] + bh, NAVY)
    label(d, X[2] + bw / 2 + 4, Y[1] + bh + 12, "commands", NAVY, anchor="start")
    path_arrow(d, [(X[2], Y[1] + 12), (X[1] + bw + g, Y[1] + 12), (X[1] + bw + g, Y[1] - 20), (X[1] + bw - 20, Y[1] - 20), (X[1] + bw - 20, Y[1])], NAVY, dash=[2, 2])
    label(d, X[1] + bw + g + 3, Y[1] - 30, "actuation requests", NAVY, anchor="start")
    arrow(d, X[0] + bw, Y[2] + m, X[1], Y[2] + m, RED)
    label(d, X[0] + bw + g, Y[2] + m + 6, "notify", RED)
    path_arrow(d, [(X[1] + 30, Y[2] + bh), (X[1] + 30, Y[1] - 12), (X[0] + bw + gap - 12, Y[1] - 12), (X[0] + bw + gap - 12, Y[0] + 14), (X[0] + bw, Y[0] + 14)], NAVY, dash=[2, 2])
    label(d, X[1] + 34, Y[1] - 22, "QUIESCE / GRANT", NAVY, anchor="start")
    return d

def power_sm():
    W, H = CW, 262
    d = Drawing(W, H)
    bw, bh = 120, 54
    gap = (W - 3 * bw) / 2
    X = [0, bw + gap, 2 * (bw + gap)]
    yt, yb = 190, 78
    st = {"NORMAL": (X[0], yt, ["Core 1 acquisition", "all qualified sensors, Wi-Fi + BLE"], TEAL, TEAL_L),
          "ENTERING LP": (X[1], yt, ["invalidate 5 V epoch, quiesce,", "stop Wi-Fi, collect ACKs"], AMBER, AMBER_L),
          "LOW POWER": (X[2], yt, ["Core 0 acquisition", "3.3 V sensors + BLE"], TEAL, TEAL_L),
          "DEGRADED": (X[1], yb, ["missing ACK: never a 2nd owner,", "rails stay invalid, reported"], RED, RED_L),
          "RESTORING": (X[2], yb, ["stable rail, lease handover,", "per-sensor warm-up"], TEAL, TEAL_L)}
    for k, (x, y, ls, e, f) in st.items():
        box(d, x, y, bw, bh, k, ls, fill=f, edge=e, size=6.9)
    m = bh / 2
    g = gap / 2
    arrow(d, X[0] + bw, yt + m, X[1], yt + m, TEAL)
    label(d, X[0] + bw + g, yt + m + 13, "power lost", TEAL, 6.8, font="UIB")
    label(d, X[0] + bw + g, yt + m + 4, "(ISR now, 50 ms", GREY, 6.3)
    label(d, X[0] + bw + g, yt + m - 11, "debounce)", GREY, 6.3)
    arrow(d, X[1] + bw, yt + m, X[2], yt + m, TEAL)
    label(d, X[1] + bw + g, yt + m + 13, "all ACKs", TEAL, 6.8, font="UIB")
    label(d, X[1] + bw + g, yt + m + 4, "<= 200 ms", GREY, 6.3)
    label(d, X[1] + bw + g, yt + m - 11, "(network <= 2 s)", GREY, 6.3)
    arrow(d, X[2] + 40, yt, X[2] + 40, yb + bh, TEAL)
    label(d, X[2] + 36, yb + bh + 18, "power stable", TEAL, 6.6, "end", "UIB")
    label(d, X[2] + 36, yb + bh + 9, "500 ms", GREY, 6.3, "end")
    arrow(d, X[2] + 84, yb + bh, X[2] + 84, yt, RED, dash=[2, 2])
    label(d, X[2] + 88, yb + bh + 18, "any loss", RED, 6.6, "start", "UIB")
    label(d, X[2] + 88, yb + bh + 9, "aborts", RED, 6.3, "start")
    arrow(d, X[1] + bw / 2, yt, X[1] + bw / 2, yb + bh, RED)
    label(d, X[1] + bw / 2 + 4, yb + bh + 13, "missing ACK", RED, 6.6, "start", "UIB")
    arrow(d, X[2], yb + m, X[1] + bw, yb + m, RED)
    label(d, X[1] + bw + g, yb + m + 4, "missing ACK", RED, 6.6, font="UIB")
    path_arrow(d, [(X[2] + bw / 2, yb), (X[2] + bw / 2, 14), (X[0] + bw / 2, 14), (X[0] + bw / 2, yt)], TEAL)
    label(d, (X[0] + X[2] + bw) / 2, 18, "restored: grant the normal worker, each sensor WARMING until valid", TEAL, 6.8)
    box(d, X[0] + 70, yb - 10, bw - 10, 64, "BOOT", ["present + valid sense config", "-> NORMAL", "absent / unknown", "-> conservative LOW POWER"],
        fill=GREY_L, edge=GREY, size=6.4, title_size=7.4)
    return d

def lease_seq():
    W, H = CW, 250
    d = Drawing(W, H)
    cols = [("PowerCoordinator", 70), ("Active worker (old owner)", W / 2), ("Inactive worker (new owner)", W - 70)]
    for name, x in cols:
        box(d, x - 62, H - 30, 124, 26, name, [], fill=BLUE_L, edge=NAVY, title_size=7.6, center=True, top_bar=False)
        d.add(Line(x, 12, x, H - 30, strokeColor=RULE, strokeWidth=1, strokeDashArray=[3, 2]))
    P0, P1, P2 = [c[1] for c in cols]
    y = H - 50
    arrow(d, P0, y, P1, y, NAVY, label="1. QUIESCE(generation g)", label_dy=4)
    y -= 30
    box(d, P1 - 92, y - 14, 184, 36, "2. Stop new operations", ["cancel capture, finish/discard transaction,", "safe pins, unregister watchdog"],
        fill=AMBER_L, edge=AMBER, title_size=7.4, size=6.6, top_bar=False)
    y -= 38
    arrow(d, P1, y, P0, y, NAVY, label="3. RELEASED(g) - then blocks, never touches context", label_dy=4)
    y -= 26
    box(d, P0 - 62, y - 14, 124, 30, "4. Verify g + ownership", ["no ACK = no grant"], fill=AMBER_L, edge=AMBER,
        title_size=7.4, size=6.6, top_bar=False)
    y -= 30
    arrow(d, P0, y, P2, y, NAVY, label="5. GRANT(g+1, allowed rail mask)", label_dy=4)
    y -= 30
    box(d, P2 - 66, y - 18, 132, 38, "6. Validate + start", ["restore allowed interfaces only,", "register watchdog, schedule"],
        fill=TEAL_L, edge=TEAL, title_size=7.4, size=6.6, top_bar=False)
    return d


def epoch_timeline():
    W, H = CW, 150
    d = Drawing(W, H)
    lx, rx = 96, W - 6
    sc = (rx - lx) / 100.0
    X = lambda t: lx + t * sc
    text(d, 0, 118, "USB / 5 V rail", 7.4, "UIB", NAVY)
    text(d, 0, 92, "Rail epoch", 7.4, "UIB", NAVY)
    text(d, 0, 66, "pH measurement", 7.4, "UIB", NAVY)
    text(d, 0, 40, "Boolean-only check", 7.4, "UIB", RED)
    text(d, 0, 16, "Epoch check", 7.4, "UIB", TEAL)
    for a, b, on in [(0, 35, True), (35, 55, False), (55, 100, True)]:
        d.add(Rect(X(a), 114, (b - a) * sc, 12, fillColor=TEAL if on else RED, strokeColor=None))
    text(d, X(45), 117, "lost", 6.8, "UIB", colors.white, "middle")
    for a, b, v in [(0, 35, "7 (valid)"), (35, 55, "8 (invalid)"), (55, 100, "9 (still invalid until coordinator restores)")]:
        d.add(Rect(X(a), 88, (b - a) * sc, 12, fillColor=GREY_L, strokeColor=RULE, strokeWidth=0.5))
        text(d, X((a + b) / 2), 91, v, 6.8, "UI", NAVY, "middle")
    d.add(Rect(X(20), 62, 60 * sc, 12, fillColor=AMBER_L, strokeColor=AMBER, strokeWidth=0.6))
    text(d, X(21), 65, "copy {valid, 7}", 6.8, "UIB", AMBER)
    text(d, X(79), 65, "re-check", 6.8, "UIB", AMBER, "end")
    text(d, X(21), 36, "before: valid", 6.8, "UI", GREY)
    text(d, X(79), 36, "after: valid (flag restored) -> WRONGLY ACCEPTED", 6.8, "UIB", RED, "end")
    text(d, X(21), 12, "before: epoch 7", 6.8, "UI", GREY)
    text(d, X(79), 12, "after: epoch 9 != 7 -> DISCARD, publish UNPOWERED", 6.8, "UIB", TEAL, "end")
    for t in (20, 80):
        d.add(Line(X(t), 6, X(t), 128, strokeColor=AMBER, strokeWidth=0.6, strokeDashArray=[2, 2]))
    return d
