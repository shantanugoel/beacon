"""Watches the agents on one machine and keeps a normalised fleet snapshot.

Two sources, in preference order:

  herdr      push-based, knows blocked/working/idle for real, and is the only
             thing that can write back into a session
  transcript polling fallback for machines without herdr; read-only, and
             deliberately never reports "blocked"

Pane text is read only for sessions where it can change what the device shows,
and never more often than `PANE_READ_INTERVAL`, because each read is an RPC
round trip into a live terminal.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import socket
import time

from . import transcripts
from .herdr import HerdrClient, HerdrError
from .model import ActionSpec, AgentRecord
from .panes import parse_pane

log = logging.getLogger("beacon.collector")

PANE_READ_INTERVAL = 2.5      # seconds between reads of the same pane
RESYNC_INTERVAL = 15.0        # full agent.list reconciliation
TRANSCRIPT_INTERVAL = 20.0    # how often to refresh cost/title from disk
STALE_AFTER = 20 * 60         # a record nobody has mentioned in this long

# Statuses whose pane content is worth re-reading.
LIVE = {"working", "blocked", "done"}

# Matches kMaxActions in the firmware's beacon_model.h - the device's array
# size, so anything past this is dropped on the wire anyway.
kMaxDeviceActions = 4


def machine_name() -> str:
    return socket.gethostname().split(".")[0]


class Collector:
    def __init__(self, machine: str | None = None,
                 herdr_socket: str | None = None) -> None:
        self.machine = machine or machine_name()
        self.herdr = HerdrClient(herdr_socket)
        self.agents: dict[str, AgentRecord] = {}
        self._pane_read_at: dict[str, float] = {}
        self._transcript_at: dict[str, float] = {}
        self._transcript_cache: dict[str, transcripts.SessionInfo] = {}
        self._dirty = asyncio.Event()
        self.using_herdr = False
        self._current_pane = ""

    # -- snapshot ---------------------------------------------------------
    def snapshot(self) -> list[AgentRecord]:
        now = time.time()
        out = []
        for record in self.agents.values():
            if now - record.seen > STALE_AFTER:
                continue
            out.append(record)
        return out

    async def wait_dirty(self, timeout: float) -> bool:
        try:
            await asyncio.wait_for(self._dirty.wait(), timeout)
        except asyncio.TimeoutError:
            return False
        self._dirty.clear()
        return True

    def _touch(self) -> None:
        self._dirty.set()

    # -- herdr ------------------------------------------------------------
    def _record_for(self, pane_id: str) -> AgentRecord:
        key = f"{self.machine}:{pane_id.replace(':', '')}"
        record = self.agents.get(key)
        if record is None:
            record = AgentRecord(id=key, machine=self.machine, pane_id=pane_id)
            record.first_seen = True
            self.agents[key] = record
        return record

    async def _apply_agent_info(self, info: dict) -> None:
        pane_id = info.get("pane_id")
        if not pane_id:
            return
        # Every pane carries an agent_status field, including plain shells.
        # Only panes herdr has actually bound an agent to are sessions.
        if not info.get("agent"):
            if self.agents.pop(f"{self.machine}:{pane_id.replace(':', '')}",
                               None) is not None:
                self._touch()
            return
        record = self._record_for(pane_id)
        before = record.digest()

        status = info.get("agent_status") or "unknown"
        if status != record.status:
            record.since = time.time()
        record.status = status
        first = getattr(record, "first_seen", False)
        record.seen = time.time()
        # A pane's own `focused` flag means "focused within its tab", so on a
        # multi-workspace setup nearly everything claims it. The device's focus
        # caret has to mean "the pane you are actually looking at", which is
        # what pane.current reports.
        record.focused = (info.get("pane_id") == self._current_pane)
        record.actionable = True

        title = (info.get("terminal_title_stripped")
                 or info.get("terminal_title") or info.get("name") or "")
        if title:
            record.title = title
        cwd = info.get("cwd") or ""
        if cwd:
            record.project = transcripts.project_name(cwd)

        session = ((info.get("agent_session") or {}).get("value")
                   if (info.get("agent_session") or {}).get("kind") == "id"
                   else None)
        await self._enrich_from_transcript(record, session, seed_since=first)
        record.first_seen = False
        await self._maybe_read_pane(record)
        self._compose_actions(record)

        if record.digest() != before:
            self._touch()

    async def _enrich_from_transcript(self, record: AgentRecord,
                                      session_id: str | None,
                                      seed_since: bool = False) -> None:
        if not session_id:
            return
        last = self._transcript_at.get(session_id, 0.0)
        if time.time() - last < TRANSCRIPT_INTERVAL:
            info = self._transcript_cache.get(session_id)
        else:
            self._transcript_at[session_id] = time.time()
            info = await asyncio.to_thread(transcripts.by_session_id, session_id)
            if info is not None:
                self._transcript_cache[session_id] = info
        if info is None:
            return
        if info.title and not record.title:
            record.title = info.title
        # "HEAD" means a detached or non-repo directory; it tells you nothing.
        if info.branch and info.branch != "HEAD":
            record.branch = info.branch
        if info.cwd and not record.project:
            record.project = transcripts.project_name(info.cwd)
        record.cost_milli = info.cost_milli or record.cost_milli
        record.lines_added = info.lines_added or record.lines_added
        record.lines_removed = info.lines_removed or record.lines_removed
        if info.tokens and not record.tokens:
            record.tokens = info.tokens
        if seed_since and info.mtime:
            # A cold-started collector has no idea how long a session has held
            # its state. The transcript's last write is the best available
            # lower bound, and beats telling you every agent just started.
            record.since = min(record.since, info.mtime)

    async def _maybe_read_pane(self, record: AgentRecord) -> None:
        if record.status not in LIVE or not record.pane_id:
            record.question = ""
            record.offered = []
            return
        now = time.time()
        if now - self._pane_read_at.get(record.pane_id, 0.0) < PANE_READ_INTERVAL:
            return
        self._pane_read_at[record.pane_id] = now
        try:
            text = await self.herdr.read_pane(record.pane_id, "visible", 60)
        except HerdrError as exc:
            log.debug("pane read failed for %s: %s", record.pane_id, exc)
            return
        reading = parse_pane(text)
        record.activity = reading.activity
        if reading.tokens:
            record.tokens = reading.tokens
        # Only a session herdr calls blocked may present answer buttons. The
        # parser is careful, but this makes a false positive impossible to act
        # on rather than merely unlikely.
        if record.status == "blocked" and reading.options:
            record.question = reading.question
            record.offered = list(reading.options)
        else:
            record.question = ""
            record.offered = []

    def _compose_actions(self, record: AgentRecord) -> None:
        """Rebuild the action list: what the agent offered, then what is always
        available. Capped at four - the device shows a short list, and a long
        one turns a glance into a menu.

        Rebuilt from `offered` rather than appended to `actions`, because this
        runs on every pane event while the pane itself is only re-read every
        few seconds; appending made "Focus this pane" pile up three deep.
        """
        extra: list[ActionSpec] = []
        if record.status == "working":
            extra.append(ActionSpec(id="interrupt", label="Interrupt"))
        if record.status in ("idle", "done"):
            extra.append(ActionSpec(id="continue", label="Continue"))
        extra.append(ActionSpec(id="focus", label="Focus this pane"))
        record.actions = (list(record.offered) + extra)[:kMaxDeviceActions]

    async def _refresh_current_pane(self) -> None:
        try:
            result = await self.herdr.call("pane.current")
        except HerdrError:
            return
        pane = (result or {}).get("pane") or {}
        current = pane.get("pane_id") or ""
        if current != self._current_pane:
            self._current_pane = current
            for record in self.agents.values():
                focused = record.pane_id == current
                if focused != record.focused:
                    record.focused = focused
                    self._touch()

    async def _resync(self) -> None:
        await self._refresh_current_pane()
        try:
            agents = await self.herdr.agents()
        except HerdrError as exc:
            log.debug("resync failed: %s", exc)
            return
        live = set()
        for info in agents:
            await self._apply_agent_info(info)
            pane = info.get("pane_id")
            if pane:
                live.add(f"{self.machine}:{pane.replace(':', '')}")
        # Panes herdr no longer lists are gone; drop them rather than let them
        # rot into "stale" and clutter the screen.
        for key in [k for k in self.agents if k not in live]:
            del self.agents[key]
            self._touch()

    async def _run_herdr(self) -> None:
        while True:
            if not self.herdr.available():
                self.using_herdr = False
                await asyncio.sleep(5)
                continue
            try:
                await self._resync()
                self.using_herdr = True
                resync = asyncio.create_task(self._resync_loop())
                try:
                    async for event in self.herdr.events():
                        kind = event.get("event", "")
                        data = event.get("data") or {}
                        if kind == "pane_focused":
                            await self._refresh_current_pane()
                        if kind in ("pane_updated", "pane_created",
                                    "pane_focused", "pane_agent_detected"):
                            pane = data.get("pane") or data
                            if pane.get("agent") or pane.get("agent_status"):
                                await self._apply_agent_info(pane)
                        elif kind in ("pane_closed", "pane_exited"):
                            await self._resync()
                finally:
                    resync.cancel()
                    with contextlib.suppress(asyncio.CancelledError):
                        await resync
            except (HerdrError, OSError) as exc:
                log.info("herdr stream ended (%s); retrying", exc)
            self.using_herdr = False
            await asyncio.sleep(3)

    async def _resync_loop(self) -> None:
        while True:
            await asyncio.sleep(RESYNC_INTERVAL)
            await self._resync()

    # -- transcript-only fallback -----------------------------------------
    async def _run_transcripts(self) -> None:
        # Let the herdr path win the race on startup; running both briefly
        # produced a screen with every session listed twice.
        for _ in range(20):
            if self.using_herdr:
                break
            if not self.herdr.available():
                break
            await asyncio.sleep(0.25)
        while True:
            if self.using_herdr:
                await asyncio.sleep(5)
                continue
            sessions = await asyncio.to_thread(transcripts.recent)
            live = set()
            for info in sessions:
                key = f"{self.machine}:{info.session_id[:8]}"
                live.add(key)
                record = self.agents.get(key)
                if record is None:
                    record = AgentRecord(id=key, machine=self.machine)
                    self.agents[key] = record
                before = record.digest()
                status = transcripts.infer_status(info)
                if status != record.status:
                    record.since = time.time()
                record.status = status
                record.seen = time.time()
                record.title = info.title or transcripts.project_name(info.cwd)
                record.project = transcripts.project_name(info.cwd)
                record.branch = "" if info.branch == "HEAD" else info.branch
                record.tokens = info.tokens
                record.cost_milli = info.cost_milli
                record.lines_added = info.lines_added
                record.lines_removed = info.lines_removed
                record.actionable = False
                record.offered = []
                record.actions = []
                if record.digest() != before:
                    self._touch()
            if not self.using_herdr:
                for key in [k for k in self.agents if k not in live]:
                    del self.agents[key]
                    self._touch()
            await asyncio.sleep(6)

    async def run(self) -> None:
        await asyncio.gather(self._run_herdr(), self._run_transcripts())

    # -- write back --------------------------------------------------------
    async def execute(self, agent_id: str, action_id: str) -> str:
        record = self.agents.get(agent_id)
        if record is None:
            return "unknown agent"
        if not record.actionable or not record.pane_id:
            return "not actionable"
        try:
            if action_id == "focus":
                await self.herdr.focus(record.pane_id)
                return "focused"
            if action_id == "interrupt":
                await self.herdr.send_keys(record.pane_id, ["esc"])
                return "interrupted"
            if action_id == "continue":
                await self.herdr.prompt(record.pane_id, "continue")
                return "sent"
            if action_id.isdigit():
                # Answer the agent's own numbered prompt. Only ever an option
                # the agent printed and the parser read back - the digit is
                # sent alone, since selecting an option confirms it and a
                # stray Enter would submit an empty turn.
                allowed = {a.id for a in record.actions if a.id.isdigit()}
                if action_id not in allowed:
                    return "option no longer offered"
                await self.herdr.send_keys(record.pane_id, [action_id])
                # The answer changes everything on screen; read it back soon.
                self._pane_read_at.pop(record.pane_id, None)
                self._touch()
                return "answered"
        except HerdrError as exc:
            return f"failed: {exc}"
        return "unsupported action"
