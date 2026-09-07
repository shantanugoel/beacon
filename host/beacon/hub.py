"""The hub: one URL for the device, N machines behind it.

Endpoints
  GET  /v1/state?rev=N&wait=25   long-poll; 200 with a new snapshot, or 304
  POST /v1/action               {"agent": id, "action": id}
  POST /v1/report               agentd pushes a machine's fleet
  GET  /v1/commands?machine=X   agentd long-polls for queued actions
  GET  /                        a plain human view, for debugging from a laptop

The device is the constrained client, so the state payload is shaped for it:
short keys, pre-sorted, pre-counted, capped at what the screen can hold, and
carrying the wall clock so the firmware needs no SNTP or timezone database.

Standard library only. Adding aiohttp to reach a 400x300 panel would be a
strange trade.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from email.utils import formatdate

from .model import AgentRecord, STATUS_RANK, sort_key

log = logging.getLogger("beacon.hub")

MAX_AGENTS = 16          # the firmware's array size; the contract, not a hint
DEFAULT_WAIT = 25.0
MAX_WAIT = 50.0


class Hub:
    def __init__(self, local=None, tz_offset_minutes: int | None = None):
        self.local = local                     # optional in-process Collector
        self.remote: dict[str, list[AgentRecord]] = {}
        self.remote_seen: dict[str, float] = {}
        self.commands: dict[str, list[dict]] = {}
        self.command_waiters: dict[str, asyncio.Event] = {}
        self.rev = 1
        self._changed = asyncio.Condition()
        self._last_digest: tuple | None = None
        self.tz_offset = tz_offset_minutes
        self.started = time.time()

    # -- state -------------------------------------------------------------
    def _all_records(self) -> list[AgentRecord]:
        records: list[AgentRecord] = []
        if self.local is not None:
            records.extend(self.local.snapshot())
        now = time.time()
        for machine, items in self.remote.items():
            if now - self.remote_seen.get(machine, 0) > 90:
                continue
            records.extend(items)
        records.sort(key=sort_key)
        return records

    def _local_time(self) -> tuple[int, int, str]:
        if self.tz_offset is None:
            t = time.localtime()
        else:
            t = time.gmtime(time.time() + self.tz_offset * 60)
        label = time.strftime("%a %d %b", t).upper()
        return t.tm_hour, t.tm_min, label

    def state(self) -> dict:
        now = time.time()
        records = self._all_records()
        counts = {k: 0 for k in STATUS_RANK}
        for r in records:
            counts[r.status] = counts.get(r.status, 0) + 1
        shown = records[:MAX_AGENTS]
        hour, minute, label = self._local_time()
        return {
            "rev": self.rev,
            "hh": hour,
            "mm": minute,
            "d": label,
            "n": {
                "w": counts.get("working", 0),
                "b": counts.get("blocked", 0),
                "d": counts.get("done", 0),
                "i": counts.get("idle", 0),
                "m": len({r.machine for r in records}),
            },
            "agents": [r.wire(now) for r in shown],
        }

    def _digest(self) -> tuple:
        records = self._all_records()
        hour, minute, _ = self._local_time()
        # The clock is part of the digest so the device's header stays honest
        # without a separate timer, but only to the minute - a per-second
        # revision would mean a panel refresh every second.
        return (hour, minute, tuple(r.digest() for r in records[:MAX_AGENTS]))

    async def bump_if_changed(self) -> None:
        digest = self._digest()
        if digest == self._last_digest:
            return
        self._last_digest = digest
        self.rev += 1
        async with self._changed:
            self._changed.notify_all()

    async def wait_for_change(self, since: int, timeout: float) -> None:
        if self.rev != since:
            return
        deadline = time.monotonic() + timeout
        async with self._changed:
            while self.rev == since:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return
                try:
                    await asyncio.wait_for(self._changed.wait(), remaining)
                except asyncio.TimeoutError:
                    return

    # -- commands ----------------------------------------------------------
    def queue_command(self, agent_id: str, action_id: str) -> str:
        machine = agent_id.split(":", 1)[0]
        if self.local is not None and agent_id in self.local.agents:
            return "local"
        self.commands.setdefault(machine, []).append(
            {"agent": agent_id, "action": action_id, "at": time.time()})
        event = self.command_waiters.get(machine)
        if event is not None:
            event.set()
        return "queued"

    async def take_commands(self, machine: str, timeout: float) -> list[dict]:
        pending = self.commands.pop(machine, [])
        if pending:
            return pending
        event = self.command_waiters.setdefault(machine, asyncio.Event())
        event.clear()
        try:
            await asyncio.wait_for(event.wait(), timeout)
        except asyncio.TimeoutError:
            return []
        return self.commands.pop(machine, [])

    async def ticker(self) -> None:
        """One place that decides the device should look again.

        Everything funnels through bump_if_changed, so a heartbeat that
        changes nothing never costs an e-ink refresh."""
        while True:
            await self.bump_if_changed()
            if self.local is not None:
                await self.local.wait_dirty(2.0)
            else:
                await asyncio.sleep(2.0)
