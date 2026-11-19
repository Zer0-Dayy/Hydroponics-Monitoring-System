# Shared styles, flowable helpers and diagram primitives for the architecture baseline PDF.
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.lib import colors
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.enums import TA_CENTER
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfbase.pdfmetrics import registerFontFamily, stringWidth
from reportlab.platypus import (Paragraph, Spacer, Table, TableStyle, KeepTogether,
                                ListFlowable, ListItem, Preformatted)
from reportlab.graphics.shapes import Drawing, Rect, String, Line, Polygon

F = "C:/Windows/Fonts/"
pdfmetrics.registerFont(TTFont("UI", F + "segoeui.ttf"))
pdfmetrics.registerFont(TTFont("UIB", F + "segoeuib.ttf"))
pdfmetrics.registerFont(TTFont("UII", F + "segoeuii.ttf"))
pdfmetrics.registerFont(TTFont("UISB", F + "seguisb.ttf"))
pdfmetrics.registerFont(TTFont("Mono", F + "consola.ttf"))
pdfmetrics.registerFont(TTFont("MonoB", F + "consolab.ttf"))
registerFontFamily("UI", normal="UI", bold="UIB", italic="UII", boldItalic="UIB")
registerFontFamily("Mono", normal="Mono", bold="MonoB", italic="Mono", boldItalic="MonoB")

NAVY = colors.HexColor("#12303f")
TEAL = colors.HexColor("#12807a")
TEAL_L = colors.HexColor("#e8f3f2")
TEAL_M = colors.HexColor("#c9e4e1")
AMBER = colors.HexColor("#c07a2c")
AMBER_L = colors.HexColor("#fbf1e4")
RED = colors.HexColor("#b4413a")
RED_L = colors.HexColor("#f9e9e7")
BLUE_L = colors.HexColor("#e9eff7")
BLUE = colors.HexColor("#2f5d8a")
GREY = colors.HexColor("#5b6670")
GREY_L = colors.HexColor("#f3f5f6")
RULE = colors.HexColor("#d5dbde")
TEXT = colors.HexColor("#1d2429")
NEW = colors.HexColor("#7a3fa0")
NEW_L = colors.HexColor("#f3ecf8")
NEW3 = colors.HexColor("#2e7d32")
NEW3_L = colors.HexColor("#e9f5ea")

PAGE_W, PAGE_H = A4
MARGIN = 18 * mm
CW = PAGE_W - 2 * MARGIN

S = {}
S["body"] = ParagraphStyle("body", fontName="UI", fontSize=9.6, leading=14, textColor=TEXT, spaceAfter=5)
S["cell"] = ParagraphStyle("cell", parent=S["body"], fontSize=8.4, leading=11.4, spaceAfter=0)
S["cellb"] = ParagraphStyle("cellb", parent=S["cell"], fontName="UIB")
S["head"] = ParagraphStyle("head", parent=S["cell"], fontName="UIB", textColor=colors.white)
S["h1"] = ParagraphStyle("h1", fontName="UIB", fontSize=20, leading=25, textColor=NAVY, spaceBefore=16, spaceAfter=8, keepWithNext=1)
S["h2"] = ParagraphStyle("h2", fontName="UIB", fontSize=12.5, leading=16, textColor=TEAL, spaceBefore=9, spaceAfter=4)
S["h3"] = ParagraphStyle("h3", fontName="UIB", fontSize=10.2, leading=13.5, textColor=NAVY, spaceBefore=6, spaceAfter=2)
S["caption"] = ParagraphStyle("caption", parent=S["body"], fontSize=8, leading=10.5, textColor=GREY, alignment=TA_CENTER, spaceBefore=2, spaceAfter=8)
S["code"] = ParagraphStyle("code", fontName="Mono", fontSize=8.1, leading=11, textColor=NAVY)
S["box"] = ParagraphStyle("box", parent=S["body"], spaceAfter=0)
S["boxt"] = ParagraphStyle("boxt", parent=S["body"], fontName="UIB", fontSize=8.4, leading=11, spaceAfter=2)
S["bullet"] = ParagraphStyle("bullet", parent=S["body"], spaceAfter=2.5)
S["toc0"] = ParagraphStyle("toc0", fontName="UISB", fontSize=10, leading=17, textColor=NAVY)
S["toc1"] = ParagraphStyle("toc1", fontName="UI", fontSize=8.8, leading=12, textColor=GREY, leftIndent=16)


def P(t, st="body"):
    return Paragraph(t, S[st] if isinstance(st, str) else st)


def H1(t, key=None):
    p = Paragraph(t, S["h1"])
    p._toc = (0, t, key)
    return p


def H2(t):
    return Paragraph(t, S["h2"])


def H3(t):
    return Paragraph(t, S["h3"])


def bullets(items, num=False):
    li = [ListItem(Paragraph(i, S["bullet"]), leftIndent=14) for i in items]
    if num:
        return ListFlowable(li, bulletType="1", bulletFontName="UIB", bulletFontSize=8.6,
                            bulletColor=TEAL, leftIndent=14, spaceAfter=4)
    return ListFlowable(li, bulletType="bullet", start="\u2022", bulletFontName="UI", bulletFontSize=9,
                        bulletColor=TEAL, leftIndent=12, spaceAfter=4)


def _box(title, body, bg, edge):
    inner = [Paragraph(f'<font color="#{edge.hexval()[2:]}">{title}</font>', S["boxt"])]
    for b in body:
        inner.append(Paragraph(b, S["box"]) if isinstance(b, str) else b)
    t = Table([[inner]], colWidths=[CW])
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), bg),
        ("LINEBEFORE", (0, 0), (0, -1), 3, edge),
        ("LEFTPADDING", (0, 0), (-1, -1), 10), ("RIGHTPADDING", (0, 0), (-1, -1), 10),
        ("TOPPADDING", (0, 0), (-1, -1), 7), ("BOTTOMPADDING", (0, 0), (-1, -1), 8),
    ]))
    return KeepTogether([t, Spacer(1, 7)])


def short(*lines):
    return _box("IN SHORT", lines, TEAL_L, TEAL)


def note(title, *lines):
    return _box(title.upper(), lines, AMBER_L, AMBER)


def new(title, *lines):
    return _box("NEW IN REV 2 \u2013 " + title.upper(), lines, NEW_L, NEW)


def new3(title, *lines):
    return _box("NEW IN REV 3 \u2013 " + title.upper(), lines, NEW3_L, NEW3)


def example(title, *lines):
    return _box("WORKED EXAMPLE \u2013 " + title.upper(), lines, BLUE_L, BLUE)


def code(text):
    t = Table([[Preformatted(text, S["code"])]], colWidths=[CW])
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), GREY_L), ("BOX", (0, 0), (-1, -1), 0.5, RULE),
        ("LEFTPADDING", (0, 0), (-1, -1), 9), ("TOPPADDING", (0, 0), (-1, -1), 6),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 7)]))
    return KeepTogether([t, Spacer(1, 7)])


def table(header, rows, widths, first_bold=True, highlight_rows=(), hl3_rows=()):
    """widths are fractions of the content width; highlight_rows / hl3_rows are 0-based body rows marked NEW in Rev 2 / Rev 3."""
    data = [[Paragraph(h, S["head"]) for h in header]]
    for r in rows:
        data.append([Paragraph(str(c), S["cellb"] if (i == 0 and first_bold) else S["cell"]) for i, c in enumerate(r)])
    t = Table(data, colWidths=[w * CW for w in widths], repeatRows=1)
    st = [
        ("BACKGROUND", (0, 0), (-1, 0), NAVY),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 6), ("RIGHTPADDING", (0, 0), (-1, -1), 6),
        ("TOPPADDING", (0, 0), (-1, -1), 4.5), ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
        ("LINEBELOW", (0, 1), (-1, -1), 0.4, RULE),
    ]
    for i in range(2, len(data), 2):
        st.append(("BACKGROUND", (0, i), (-1, i), GREY_L))
    for i in highlight_rows:
        st.append(("BACKGROUND", (0, i + 1), (-1, i + 1), NEW_L))
        st.append(("LINEBEFORE", (0, i + 1), (0, i + 1), 3, NEW))
    for i in hl3_rows:
        st.append(("BACKGROUND", (0, i + 1), (-1, i + 1), NEW3_L))
        st.append(("LINEBEFORE", (0, i + 1), (0, i + 1), 3, NEW3))
    t.setStyle(TableStyle(st))
    return t


def caption(t):
    return Paragraph(t, S["caption"])


# ---------------------------------------------------------------- drawing primitives
def wrap(text, font, size, width):
    out = []
    for para in text.split("\n"):
        line = ""
        for w in para.split(" "):
            cand = (line + " " + w).strip()
            if stringWidth(cand, font, size) <= width:
                line = cand
            else:
                if line:
                    out.append(line)
                line = w
        out.append(line)
    return out


def box(d, x, y, w, h, title, lines=(), fill=TEAL_L, edge=TEAL, title_size=8.6, size=7.4,
        top_bar=True, title_color=NAVY, center=False):
    """x, y = bottom-left corner."""
    d.add(Rect(x, y, w, h, fillColor=fill, strokeColor=RULE if top_bar else edge, strokeWidth=0.6, rx=3, ry=3))
    if top_bar:
        d.add(Rect(x, y + h - 3, w, 3, fillColor=edge, strokeColor=None))
    ty = y + h - 6 - title_size
    anchor, tx = ("middle", x + w / 2) if center else ("start", x + 6)
    if title:
        for tl in wrap(title, "UIB", title_size, w - 10):
            d.add(String(tx, ty, tl, fontName="UIB", fontSize=title_size, fillColor=title_color, textAnchor=anchor))
            ty -= title_size + 2
        ty -= 1
    for ln in lines:
        for tl in wrap(ln, "UI", size, w - 12):
            d.add(String(tx, ty, tl, fontName="UI", fontSize=size, fillColor=GREY, textAnchor=anchor))
            ty -= size + 2.2


def arrow(d, x1, y1, x2, y2, color=TEAL, width=1.1, head=5, label=None, label_dx=0, label_dy=3, dash=None, size=6.8):
    import math
    ln = Line(x1, y1, x2, y2, strokeColor=color, strokeWidth=width)
    if dash:
        ln.strokeDashArray = dash
    d.add(ln)
    a = math.atan2(y2 - y1, x2 - x1)
    p1 = (x2 - head * math.cos(a - 0.4), y2 - head * math.sin(a - 0.4))
    p2 = (x2 - head * math.cos(a + 0.4), y2 - head * math.sin(a + 0.4))
    d.add(Polygon([x2, y2, p1[0], p1[1], p2[0], p2[1]], fillColor=color, strokeColor=color, strokeWidth=0.5))
    if label:
        d.add(String((x1 + x2) / 2 + label_dx, (y1 + y2) / 2 + label_dy, label, fontName="UI", fontSize=size,
                     fillColor=color, textAnchor="middle"))


def text(d, x, y, t, size=7.4, font="UI", color=GREY, anchor="start"):
    d.add(String(x, y, t, fontName=font, fontSize=size, fillColor=color, textAnchor=anchor))


def path_arrow(d, pts, color=TEAL, width=1.1, dash=None, head=5):
    """Orthogonal/polyline arrow through pts; arrowhead on the last segment."""
    for (x1, y1), (x2, y2) in zip(pts[:-2], pts[1:-1]):
        ln = Line(x1, y1, x2, y2, strokeColor=color, strokeWidth=width)
        if dash:
            ln.strokeDashArray = dash
        d.add(ln)
    (x1, y1), (x2, y2) = pts[-2], pts[-1]
    arrow(d, x1, y1, x2, y2, color, width, head, dash=dash)


def label(d, x, y, t, color=GREY, size=6.8, anchor="middle", font="UI"):
    text(d, x, y, t, size, font, color, anchor)
