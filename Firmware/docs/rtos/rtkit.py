# Status tags and boxes specific to the RTOS design document (house style from ../architecture/kit.py).
import kit
from figs import *  # noqa: F401,F403  (re-exports kit + status colours)

TAG = {
    "done": ("IMPLEMENTED", "#2e7d32"),
    "plan": ("STAGE 4", "#2f5d8a"),
    "later": ("LATER", "#5b6670"),
    "name": ("PLANNED NAME", "#7a3fa0"),
}


def tag(kind):
    t, c = TAG[kind]
    return f'<font name="UIB" size="6.6" color="{c}">[{t}]</font>'


def done(t):
    return tag("done") + " " + t


def plan(t):
    return tag("plan") + " " + t


def later(t):
    return tag("later") + " " + t


def pname(t):
    """A file or symbol that does not exist yet."""
    return f'<font name="Mono">{t}</font> ' + tag("name")


def m(t):
    """Inline code."""
    return f'<font name="Mono" size="8.6">{t}</font>'


def rule(title, *lines):
    return kit._box("RULE – " + title.upper(), lines, RED_L, RED)


def why(title, *lines):
    return kit._box("WHY – " + title.upper(), lines, BLUE_L, BLUE)


def status_box(*lines):
    return kit._box("STATUS", lines, NEW3_L, NEW3)
