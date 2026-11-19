"""Build the RTOS architecture PDF.

Usage: python build.py [output.pdf]  (default: ../../RTOS Architecture.pdf)
Needs ReportLab and the Segoe UI / Consolas fonts in C:/Windows/Fonts.
Reuses the style kit and some figures from ../architecture.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(1, str(HERE.parent / "architecture"))
from rtkit import *  # noqa: E402
from reportlab.platypus import BaseDocTemplate, PageTemplate, Frame, CondPageBreak, KeepTogether  # noqa: E402
from reportlab.platypus import Paragraph as _P  # noqa: E402
from reportlab.platypus.tableofcontents import TableOfContents  # noqa: E402
from reportlab.graphics.shapes import Drawing as _D  # noqa: E402
import content_1 as A, content_2 as B, content_3 as C  # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else str(HERE.parents[1] / "RTOS Architecture.pdf")
DATE = "25 October 2026"
FOOT = DATE + "  |  Stage 4 design – not yet implemented unless tagged IMPLEMENTED"


class Doc(BaseDocTemplate):
    def afterFlowable(self, f):
        t = getattr(f, "_toc", None)
        if t:
            lvl, txt, key = t
            self.canv.bookmarkPage(key)
            self.canv.addOutlineEntry(txt, key, level=lvl, closed=False)
            self.notify("TOCEntry", (lvl, txt, self.page, key))


def cover(c, d):
    c.saveState()
    c.setFillColor(NAVY); c.rect(0, PAGE_H - 58 * mm, PAGE_W, 58 * mm, fill=1, stroke=0)
    c.setFillColor(TEAL); c.rect(0, PAGE_H - 60 * mm, PAGE_W, 2 * mm, fill=1, stroke=0)
    c.setFillColor(colors.white); c.setFont("UIB", 11)
    c.drawString(MARGIN, PAGE_H - 22 * mm, "HYDROPONICS MONITORING SYSTEM")
    c.setFont("UI", 10); c.drawString(MARGIN, PAGE_H - 29 * mm, "FreeRTOS design – Stage 4 (WP7 + WP10)")
    c.setFont("UIB", 34); c.drawRightString(PAGE_W - MARGIN, PAGE_H - 45 * mm, "RTOS")
    c.setFillColor(GREY); c.setFont("UI", 7.4)
    c.drawString(MARGIN, 9 * mm, FOOT)
    c.restoreState()


def body(c, d):
    c.saveState()
    c.setFillColor(TEAL); c.rect(0, PAGE_H - 5, PAGE_W, 5, fill=1, stroke=0)
    c.setFont("UIB", 7.4); c.setFillColor(GREY)
    c.drawString(MARGIN, PAGE_H - 13 * mm, "HYDROPONICS / RTOS ARCHITECTURE")
    c.setFont("UI", 7.4)
    c.drawRightString(PAGE_W - MARGIN, PAGE_H - 13 * mm, "ESP32-S3-WROOM-1-N16R8  |  REV 1")
    c.setStrokeColor(RULE); c.setLineWidth(0.5)
    c.line(MARGIN, PAGE_H - 14.8 * mm, PAGE_W - MARGIN, PAGE_H - 14.8 * mm)
    c.line(MARGIN, 13 * mm, PAGE_W - MARGIN, 13 * mm)
    c.drawString(MARGIN, 9 * mm, FOOT)
    c.drawRightString(PAGE_W - MARGIN, 9 * mm, "Page %d" % d.page)
    c.restoreState()


doc = Doc(OUT, pagesize=A4, leftMargin=MARGIN, rightMargin=MARGIN, topMargin=20 * mm, bottomMargin=17 * mm,
          title="RTOS Architecture - Rev 1", author="Hydroponics Monitoring System project",
          subject="FreeRTOS tasks, scheduling, channels, Safety and supervision for the ESP32-S3 firmware")
fr = Frame(MARGIN, 17 * mm, CW, PAGE_H - 37 * mm, id="f", leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
doc.addPageTemplates([PageTemplate("cover", [fr], onPage=cover), PageTemplate("body", [fr], onPage=body)])

S["h2"].keepWithNext = 1  # a heading never ends a page above its figure or table
toc = TableOfContents(levelStyles=[S["toc0"], S["toc1"]], dotsMinLevel=0)
story = A.front() + A.toc_page(toc)
for fn in [A.about, A.primer, A.platform, A.tasks, A.boot,
           B.acquisition, B.safety, B.channels, B.interrupts,
           C.timing, C.diagnostics, C.memory, C.failures, C.later, C.implementation, C.acceptance, C.team_rules]:
    story += fn()

merged = []
for f in story:
    if merged and isinstance(f, _P) and f.style.name == "caption" and isinstance(merged[-1], _D):
        merged[-1] = KeepTogether([merged[-1], f])
    else:
        merged.append(f)
fixed = []
i = 0
while i < len(merged):
    f = merged[i]
    if (isinstance(f, _P) and f.style.name in ("h2", "h3") and i + 1 < len(merged)
            and not isinstance(merged[i + 1], Table)):  # tables split cleanly; only figures/boxes are glued
        fixed.append(CondPageBreak(110))
        fixed.append(KeepTogether([f, merged[i + 1]]))  # never strand a heading above its figure or table
        i += 2
        continue
    fixed.append(f)
    i += 1
doc.multiBuild(fixed)
print("built", OUT)
