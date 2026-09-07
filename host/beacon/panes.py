"""Turning raw terminal text into the two strings the device shows.

The device has room for one line of "what is it doing" and, when an agent is
blocked, one short question plus the options it is actually offering. Getting
those from the pane rather than inventing them is what makes the answer keys
correct: BEACON never guesses that "1" means yes, it reads the numbered list
the agent printed and sends back the digit next to the label you picked.
"""

from __future__ import annotations

import re
import unicodedata

from .model import ActionSpec

# Leading box drawing, gutter glyphs and the selection caret.
_GUTTER = re.compile(r"^[\s│┃┊┆╎║|]*")
_CARET = re.compile(r"^[❯>▶→*]\s*")
_OPTION = re.compile(r"^(\d{1,2})[.)]\s+(.*\S)\s*$")
# "Bullet" lines are Claude Code's narration and tool headers.
_BULLET = re.compile(r"^[●•○✘✗✓]\s+(.*\S)\s*$")
# "· 3s" / "· 1m 4s" trailing timing on a tool header.
_TRAILING_TIME = re.compile(r"\s*[··]\s*\d+(?:m\s*\d+)?s\s*[….]*\s*$")
# The animated status line, e.g. "✽ Gusting… (4m 14s · ↓ 6.6k tokens)".
_SPINNER = re.compile(
    r"^[✶✸✻✽✴✳✵✷·*✦✧]\s+"
    r"(\w[\w '-]*)[….]{0,3}\s*\((.*?)\)\s*$"
)
_TOKENS = re.compile(r"([\d.]+)\s*([kKmM])?\s*tokens")
_ESC = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")

_QUESTION_HINTS = (
    "do you want",
    "would you like",
    "proceed?",
    "continue?",
    "confirm",
    "allow this",
    "approve",
)


def _clean(line: str) -> str:
    line = _ESC.sub("", line)
    line = _GUTTER.sub("", line)
    # Trailing box edge.
    line = re.sub(r"[\s│┃║|]+$", "", line)
    return line


def _printable(text: str) -> str:
    """Drop control and format characters but keep normal punctuation."""
    return "".join(
        ch for ch in text
        if ch == " " or unicodedata.category(ch)[0] not in ("C", "Z")
    ).strip()


def _squash(text: str, limit: int) -> str:
    text = re.sub(r"\s+", " ", _printable(text)).strip()
    if len(text) <= limit:
        return text
    return text[: limit - 1].rstrip() + "…"


class PaneReading:
    """What one read of a pane told us."""

    def __init__(self) -> None:
        self.activity: str = ""
        self.question: str = ""
        self.options: list[ActionSpec] = []
        self.tokens: int = 0
        self.elapsed: str = ""

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return (
            f"PaneReading(activity={self.activity!r}, question={self.question!r}, "
            f"options={[(o.id, o.label) for o in self.options]}, tokens={self.tokens})"
        )


def _parse_tokens(inside: str) -> int:
    m = _TOKENS.search(inside)
    if not m:
        return 0
    value = float(m.group(1))
    suffix = (m.group(2) or "").lower()
    if suffix == "k":
        value *= 1_000
    elif suffix == "m":
        value *= 1_000_000
    return int(value)


def parse_pane(text: str) -> PaneReading:
    out = PaneReading()
    if not text:
        return out
    lines = [_clean(ln) for ln in text.splitlines()]

    # ---- the option block ------------------------------------------------
    # Scan from the bottom: the live prompt is always the last one printed.
    # A run of consecutive numbered lines starting at 1 is the offer.
    options: list[tuple[int, str]] = []
    block_start = -1
    for i in range(len(lines) - 1, -1, -1):
        body = _CARET.sub("", lines[i].strip())
        m = _OPTION.match(body)
        if m:
            options.append((int(m.group(1)), m.group(2)))
            block_start = i
            continue
        if options:
            # A blank line inside a box does not end the block; anything else does.
            if lines[i].strip() == "" and len(options) < 2:
                continue
            break

    options.reverse()
    numbers = [n for n, _ in options]

    # Three guards, because a wrong button here sends a real keystroke to a
    # real session. A numbered list only becomes an offer when it is
    # (a) contiguous from 1, (b) sitting at the live end of the pane, not
    # buried in scrollback, and (c) introduced by something that reads as a
    # question. Prose like "Steps: 1. build 2. flash" fails (c).
    tail_start = 0
    seen = 0
    for i in range(len(lines) - 1, -1, -1):
        if lines[i].strip():
            seen += 1
        if seen >= 16:
            tail_start = i
            break
    contiguous = numbers == list(range(1, len(numbers) + 1))
    at_live_end = block_start >= tail_start

    intro = ""
    if options and at_live_end:
        for i in range(block_start - 1, max(-1, block_start - 14), -1):
            body = _printable(lines[i])
            if not body:
                continue
            low = body.lower()
            if any(h in low for h in _QUESTION_HINTS) or body.endswith("?"):
                intro = body
                break

    if len(options) >= 2 and contiguous and at_live_end and intro:
        for number, label in options:
            # Strip the parenthetical key hints Claude Code appends.
            label = re.sub(r"\s*\((?:esc|enter|tab)[^)]*\)\s*$", "", label,
                           flags=re.I)
            out.options.append(
                ActionSpec(id=str(number), label=_squash(label, 26))
            )

        out.question = _squash(intro, 110)

        # A bare "Do you want to proceed?" says nothing about *what*. Pull the
        # most specific line above it - the command or file under review - and
        # lead with that, since it is the thing you actually need to judge.
        subject = ""
        for i in range(block_start - 1, max(-1, block_start - 12), -1):
            body = _printable(lines[i])
            if not body or body == intro:
                continue
            if len(body) < 4 or body.lower().startswith(("do you", "would you")):
                continue
            subject = body
            break
        if subject:
            out.question = _squash(f"{subject} — {intro}", 110)

    # ---- what it is doing -------------------------------------------------
    for line in reversed(lines):
        stripped = line.strip()
        m = _SPINNER.match(stripped)
        if m and not out.elapsed:
            inside = m.group(2)
            out.tokens = _parse_tokens(inside)
            out.elapsed = inside.split("·")[0].strip()
            continue
        if out.activity:
            continue
        b = _BULLET.match(stripped)
        if b:
            body = _TRAILING_TIME.sub("", b.group(1))
            if body:
                out.activity = _squash(body, 76)

    if not out.activity and out.elapsed:
        out.activity = f"working · {out.elapsed}"
    return out
