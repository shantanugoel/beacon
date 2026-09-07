"""Thin async client for the herdr JSON-RPC socket.

herdr is newline-delimited JSON over a unix socket. Two connection styles are
used: short-lived request/response sockets for calls, and one long-lived socket
holding an events.subscribe so status changes arrive as a push rather than a
poll.
"""

from __future__ import annotations

import asyncio
import contextlib
import itertools
import json
import os
import pathlib
from typing import AsyncIterator, Any

DEFAULT_SOCKET = pathlib.Path.home() / ".config" / "herdr" / "herdr.sock"

# Global pane events; the per-pane ones need a pane_id up front, which is no
# use when the whole point is to discover panes as they appear.
SUBSCRIPTIONS = [
    {"type": "pane.updated"},
    {"type": "pane.created"},
    {"type": "pane.closed"},
    {"type": "pane.exited"},
    {"type": "pane.agent_detected"},
    {"type": "pane.focused"},
]


class HerdrError(RuntimeError):
    pass


class HerdrClient:
    def __init__(self, socket_path: os.PathLike | str | None = None) -> None:
        self.path = pathlib.Path(socket_path or DEFAULT_SOCKET)
        self._ids = itertools.count(1)

    def available(self) -> bool:
        return self.path.exists()

    async def call(self, method: str, params: dict | None = None,
                   timeout: float = 5.0) -> Any:
        request = {
            "id": f"beacon-{next(self._ids)}",
            "method": method,
            "params": params or {},
        }
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_unix_connection(str(self.path)), timeout)
        except (OSError, asyncio.TimeoutError) as exc:
            raise HerdrError(f"cannot reach herdr at {self.path}: {exc}") from exc
        try:
            writer.write((json.dumps(request) + "\n").encode())
            await writer.drain()
            line = await asyncio.wait_for(reader.readline(), timeout)
        finally:
            writer.close()
            with contextlib.suppress(Exception):
                await writer.wait_closed()
        if not line:
            raise HerdrError(f"{method}: connection closed with no reply")
        payload = json.loads(line)
        if "error" in payload:
            raise HerdrError(f"{method}: {payload['error'].get('message')}")
        return payload.get("result")

    async def agents(self) -> list[dict]:
        result = await self.call("agent.list")
        return (result or {}).get("agents", [])

    async def read_pane(self, pane_id: str, source: str = "visible",
                        lines: int = 60) -> str:
        result = await self.call(
            "agent.read",
            {"target": pane_id, "source": source, "lines": lines,
             "strip_ansi": True},
        )
        return ((result or {}).get("read") or {}).get("text", "")

    async def send_keys(self, pane_id: str, keys: list[str]) -> None:
        await self.call("agent.send_keys", {"target": pane_id, "keys": keys})

    async def prompt(self, pane_id: str, text: str) -> None:
        await self.call("agent.prompt", {"target": pane_id, "text": text})

    async def focus(self, pane_id: str) -> None:
        await self.call("agent.focus", {"target": pane_id})

    async def notify(self, title: str, body: str = "") -> None:
        await self.call("notification.show", {"title": title, "body": body})

    async def events(self) -> AsyncIterator[dict]:
        """Yield pane events until the socket drops. Reconnection is the
        caller's problem, so it can also decide how to resync state."""
        reader, writer = await asyncio.open_unix_connection(str(self.path))
        try:
            writer.write((json.dumps({
                "id": "beacon-sub",
                "method": "events.subscribe",
                "params": {"subscriptions": SUBSCRIPTIONS},
            }) + "\n").encode())
            await writer.drain()
            while True:
                line = await reader.readline()
                if not line:
                    return
                try:
                    payload = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if "error" in payload:
                    raise HerdrError(str(payload["error"]))
                result = payload.get("result") or payload
                if isinstance(result, dict) and "event" in result:
                    yield result
        finally:
            writer.close()
            with contextlib.suppress(Exception):
                await writer.wait_closed()
