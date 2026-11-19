"""Build the firmware architecture baseline PDF.

Usage: python build.py [output.pdf]  (default: ../../Firmware Architecture.pdf)
Needs ReportLab and the Segoe UI / Consolas fonts in C:/Windows/Fonts.
"""
import sys
from pathlib import Path
from kit import *
from reportlab.platypus import BaseDocTemplate, PageTemplate, Frame
from reportlab.platypus.tableofcontents import TableOfContents
import content_a as A, content_b as B, content_c as C

OUT = sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).resolve().parents[2] / "Firmware Architecture.pdf")
DATE = "22 October 2026"


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
    c.setFont("UI", 10); c.drawString(MARGIN, PAGE_H - 29 * mm, "Firmware architecture – project baseline")
    c.setFont("UIB", 34); c.drawRightString(PAGE_W - MARGIN, PAGE_H - 45 * mm, "REV 3")
    c.setFillColor(GREY); c.setFont("UI", 7.4)
    c.drawString(MARGIN, 9 * mm, DATE + "  |  Planning baseline – validation gates remain")
    c.restoreState()


def body(c, d):
    c.saveState()
    c.setFillColor(TEAL); c.rect(0, PAGE_H - 5, PAGE_W, 5, fill=1, stroke=0)
    c.setFont("UIB", 7.4); c.setFillColor(GREY)
    c.drawString(MARGIN, PAGE_H - 13 * mm, "HYDROPONICS / FIRMWARE ARCHITECTURE BASELINE")
    c.setFont("UI", 7.4)
    c.drawRightString(PAGE_W - MARGIN, PAGE_H - 13 * mm, "ESP32-S3-WROOM-1-N16R8  |  REV 3")
    c.setStrokeColor(RULE); c.setLineWidth(0.5)
    c.line(MARGIN, PAGE_H - 14.8 * mm, PAGE_W - MARGIN, PAGE_H - 14.8 * mm)
    c.line(MARGIN, 13 * mm, PAGE_W - MARGIN, 13 * mm)
    c.drawString(MARGIN, 9 * mm, DATE + "  |  Planning baseline – validation gates remain")
    c.drawRightString(PAGE_W - MARGIN, 9 * mm, "Page %d" % d.page)
    c.restoreState()


doc = Doc(OUT, pagesize=A4, leftMargin=MARGIN, rightMargin=MARGIN, topMargin=20 * mm, bottomMargin=17 * mm,
          title="Firmware Architecture Baseline - Rev 3", author="Hydroponics Monitoring System project",
          subject="ESP32-S3-WROOM-1-N16R8 firmware architecture baseline")
fr = Frame(MARGIN, 17 * mm, CW, PAGE_H - 37 * mm, id="f", leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
doc.addPageTemplates([PageTemplate("cover", [fr], onPage=cover), PageTemplate("body", [fr], onPage=body)])

toc = TableOfContents(levelStyles=[S["toc0"], S["toc1"]], dotsMinLevel=0)
story = A.front() + A.toc_page(toc)
for fn in [A.glance, A.reading, A.requirements, A.inherited, A.hardware, A.interrupts, A.layered, A.tasks,
           B.schedule, B.datamodel, B.sync, B.safety, B.power, B.rails, B.lowpower,
           C.sensors, C.comms, C.faults, C.stages, C.decisions, C.evidence, C.references]:
    story += fn()
from reportlab.platypus import CondPageBreak, Paragraph as _P
from reportlab.platypus import KeepTogether as _KT
from reportlab.graphics.shapes import Drawing as _D
merged = []
for f in story:
    if merged and isinstance(f, _P) and f.style.name == "caption" and isinstance(merged[-1], _D):
        merged[-1] = _KT([merged[-1], f])
    else:
        merged.append(f)
fixed = []
for f in merged:
    if isinstance(f, _P) and f.style.name in ("h2", "h3"):
        fixed.append(CondPageBreak(110))
    fixed.append(f)
doc.multiBuild(fixed)
print("built", OUT)
