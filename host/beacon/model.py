"""The record the whole system agrees on.

Deliberately flat and small: it is JSON-serialised on every poll and parsed on
a microcontroller, so every field here costs bytes on the wire and cycles in
cJSON. Anything the device cannot draw does not belong in it.
"""

from __future__ import annotations

import dataclasses
import time
from typing import Literal

Status = Literal["unknown", "idle", "working", "blocked", "done", "stale"]

# Ordering used everywhere the fleet is presented: the thing that needs a human
# comes first, then live work, then results, then the quiet tail.
STATUS_RANK: dict[str, int] = {
    "blocked": 0,
    "working": 1,
    "done": 2,
    "idle": 3,
    "unknown": 4,
    "stale": 5,
}


@dataclasses.dataclass
class ActionSpec:
    """A button the device can offer for this agent.

    `id` is what comes back on POST /v1/action; how it is executed is entirely
    the collector's business, so the firmware never learns about tmux keys or
    permission-prompt layouts.
    """

    id: str
    label: str


@dataclasses.dataclass
class AgentRecord:
    id: str                  # "<machine>:<pane>" - stable across restarts
    machine: str
    title: str = ""
    project: str = ""
    branch: str = ""
    activity: str = ""
    question: str = ""
    status: Status = "unknown"
    since: float = dataclasses.field(default_factory=time.time)  # epoch of last change
    tokens: int = 0
    cost_milli: int = 0
    lines_added: int = 0
    lines_removed: int = 0
    focused: bool = False
    actionable: bool = False
    actions: list[ActionSpec] = dataclasses.field(default_factory=list)
    # Local bookkeeping, never sent to the device.
    # `offered` holds only the options parsed from the agent's own prompt.
    # `actions` is rebuilt from it each pass; keeping the two apart is what
    # stops the always-available actions accumulating on every update.
    offered: list[ActionSpec] = dataclasses.field(default_factory=list)
    seen: float = dataclasses.field(default_factory=time.time)
    pane_id: str = ""
    first_seen: bool = False

    def wire(self, now: float) -> dict:
        d = {
            "id": self.id,
            "m": self.machine,
            "t": self.title,
            "p": self.project,
            "b": self.branch,
            "s": self.status,
            "age": max(0, int(now - self.since)),
        }
        if self.activity:
            d["a"] = self.activity
        if self.question:
            d["q"] = self.question
        if self.tokens:
            d["tk"] = self.tokens
        if self.cost_milli:
            d["c"] = self.cost_milli
        if self.lines_added or self.lines_removed:
            d["dl"] = [self.lines_added, self.lines_removed]
        if self.focused:
            d["f"] = 1
        if self.actionable:
            d["w"] = 1
        if self.actions:
            d["ax"] = [[a.id, a.label] for a in self.actions]
        return d

    def digest(self) -> tuple:
        """Everything that should make the device redraw.

        `since` is included but `seen` is not: a heartbeat that changes nothing
        must not burn an e-ink refresh.
        """
        return (
            self.id, self.machine, self.title, self.project, self.branch,
            self.activity, self.question, self.status, int(self.since),
            self.tokens, self.cost_milli, self.lines_added, self.lines_removed,
            self.focused, self.actionable,
            tuple((a.id, a.label) for a in self.actions),
        )


def sort_key(a: AgentRecord) -> tuple:
    """Blocked first and longest-waiting on top; everything else newest first."""
    rank = STATUS_RANK.get(a.status, 9)
    if a.status == "blocked":
        return (rank, a.since)          # oldest block = most urgent
    return (rank, -a.since)
