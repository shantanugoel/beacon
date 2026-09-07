"""Claude Code transcript enrichment.

herdr knows an agent's *state*; the transcript knows its *history* - the
AI-generated title, the branch, and what the session has cost. Reading it is
cheap because everything wanted lives in a handful of record types, and the
tail is enough for the rest.

This module is also the fallback path: on a machine without herdr it can list
sessions on its own, inferring status from the shape of the tail.
"""

from __future__ import annotations

import json
import os
import pathlib
import time
from typing import Iterator

PROJECTS = pathlib.Path.home() / ".claude" / "projects"

# Reading the whole of a long transcript to find a title is wasteful; the
# records we want are sparse but present throughout, so we read the head for
# identity and the tail for state.
_HEAD_BYTES = 96_000
_TAIL_BYTES = 256_000


def _iter_json(blob: str) -> Iterator[dict]:
    for line in blob.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(record, dict):
            yield record


def _read_ends(path: pathlib.Path) -> tuple[str, str]:
    size = path.stat().st_size
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        head = fh.read(_HEAD_BYTES)
        if size <= _HEAD_BYTES:
            return head, ""
        fh.seek(max(0, size - _TAIL_BYTES))
        tail = fh.read()
    # The first line after a seek is almost certainly cut in half.
    return head, tail.split("\n", 1)[-1]


class SessionInfo:
    __slots__ = ("session_id", "path", "title", "cwd", "branch", "tokens",
                 "cost_milli", "lines_added", "lines_removed", "mtime",
                 "last_role", "last_is_tool_use")

    def __init__(self, session_id: str, path: pathlib.Path) -> None:
        self.session_id = session_id
        self.path = path
        self.title = ""
        self.cwd = ""
        self.branch = ""
        self.tokens = 0
        self.cost_milli = 0
        self.lines_added = 0
        self.lines_removed = 0
        self.mtime = 0.0
        self.last_role = ""
        self.last_is_tool_use = False


def load(path: pathlib.Path) -> SessionInfo | None:
    try:
        head, tail = _read_ends(path)
        mtime = path.stat().st_mtime
    except OSError:
        return None

    info = SessionInfo(path.stem, path)
    info.mtime = mtime

    for blob in (head, tail):
        for record in _iter_json(blob):
            kind = record.get("type")
            if kind == "ai-title" and record.get("aiTitle"):
                info.title = str(record["aiTitle"]).strip()
            elif kind == "cost-state":
                info.cost_milli = int(round(
                    float(record.get("totalCostUSD") or 0) * 1000))
                info.lines_added = int(record.get("totalLinesAdded") or 0)
                info.lines_removed = int(record.get("totalLinesRemoved") or 0)
                for usage in (record.get("modelUsage") or {}).values():
                    if isinstance(usage, dict):
                        info.tokens += int(usage.get("inputTokens") or 0)
                        info.tokens += int(usage.get("outputTokens") or 0)
            if record.get("cwd"):
                info.cwd = record["cwd"]
            if record.get("gitBranch"):
                info.branch = record["gitBranch"]
            if kind in ("assistant", "user"):
                info.last_role = kind
                content = (record.get("message") or {}).get("content")
                if kind == "assistant" and isinstance(content, list):
                    info.last_is_tool_use = any(
                        isinstance(b, dict) and b.get("type") == "tool_use"
                        for b in content
                    )
    return info


def by_session_id(session_id: str) -> SessionInfo | None:
    """Locate a transcript by its session UUID across all project slugs."""
    if not session_id or not PROJECTS.is_dir():
        return None
    for project in PROJECTS.iterdir():
        candidate = project / f"{session_id}.jsonl"
        if candidate.is_file():
            return load(candidate)
    return None


def recent(max_age_s: float = 6 * 3600, limit: int = 32) -> list[SessionInfo]:
    """Every session touched recently, newest first.

    Used only on machines with no herdr to talk to.
    """
    if not PROJECTS.is_dir():
        return []
    now = time.time()
    found: list[SessionInfo] = []
    for project in PROJECTS.iterdir():
        if not project.is_dir():
            continue
        for path in project.glob("*.jsonl"):
            try:
                if now - path.stat().st_mtime > max_age_s:
                    continue
            except OSError:
                continue
            info = load(path)
            if info is not None:
                found.append(info)
    found.sort(key=lambda s: s.mtime, reverse=True)
    return found[:limit]


def infer_status(info: SessionInfo, now: float | None = None) -> str:
    """Best-effort state from the shape of the transcript tail.

    Deliberately conservative: this path can never see a permission prompt, so
    it never reports "blocked". Claiming an agent needs you when it does not is
    worse than saying nothing - it trains you to ignore the device.
    """
    now = now or time.time()
    idle_for = now - info.mtime
    if idle_for > 30 * 60:
        return "stale"
    if info.last_role == "assistant" and info.last_is_tool_use:
        return "working" if idle_for < 90 else "unknown"
    if info.last_role == "assistant":
        return "done" if idle_for < 10 * 60 else "idle"
    if info.last_role == "user":
        return "working" if idle_for < 90 else "idle"
    return "unknown"


def project_name(cwd: str) -> str:
    return os.path.basename(cwd.rstrip("/")) or cwd or "—"
