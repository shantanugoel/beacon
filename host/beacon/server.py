"""asyncio HTTP front end for the Hub. Standard library only."""

from __future__ import annotations

import asyncio
import json
import logging
import time
import urllib.parse

from .hub import DEFAULT_WAIT, MAX_WAIT, Hub
from .model import AgentRecord, ActionSpec

log = logging.getLogger("beacon.server")

_STATUS_TEXT = {200: "OK", 204: "No Content", 304: "Not Modified",
                400: "Bad Request", 404: "Not Found", 405: "Method Not Allowed",
                500: "Internal Server Error"}


class Request:
    __slots__ = ("method", "path", "query", "headers", "body")

    def __init__(self, method, path, query, headers, body):
        self.method = method
        self.path = path
        self.query = query
        self.headers = headers
        self.body = body

    def json(self) -> dict:
        try:
            return json.loads(self.body or b"{}")
        except json.JSONDecodeError:
            return {}

    def q(self, key, default=None):
        values = self.query.get(key)
        return values[0] if values else default


async def _read_request(reader: asyncio.StreamReader) -> Request | None:
    line = await reader.readline()
    if not line:
        return None
    try:
        method, target, _ = line.decode("latin-1").split()
    except ValueError:
        return None
    headers = {}
    while True:
        header = await reader.readline()
        if header in (b"\r\n", b"\n", b""):
            break
        name, _, value = header.decode("latin-1").partition(":")
        headers[name.strip().lower()] = value.strip()
    body = b""
    length = int(headers.get("content-length") or 0)
    if length:
        body = await reader.readexactly(length)
    parsed = urllib.parse.urlsplit(target)
    return Request(method, parsed.path, urllib.parse.parse_qs(parsed.query),
                   headers, body)


def _respond(writer, status: int, body: bytes = b"",
             content_type: str = "application/json") -> None:
    head = [
        f"HTTP/1.1 {status} {_STATUS_TEXT.get(status, 'OK')}",
        f"Content-Length: {len(body)}",
        "Connection: close",
        "Cache-Control: no-store",
    ]
    if body:
        head.append(f"Content-Type: {content_type}")
    writer.write(("\r\n".join(head) + "\r\n\r\n").encode("latin-1"))
    if body:
        writer.write(body)


def _json(writer, status: int, payload) -> None:
    # Compact separators matter here: this is parsed on a microcontroller and
    # every space is a byte of PSRAM and a byte of radio time.
    body = json.dumps(payload, separators=(",", ":")).encode()
    _respond(writer, status, body)


class Server:
    def __init__(self, hub: Hub, host: str = "0.0.0.0", port: int = 8787,
                 token: str | None = None) -> None:
        self.hub = hub
        self.host = host
        self.port = port
        self.token = token

    def _authorised(self, request: Request) -> bool:
        if not self.token:
            return True
        header = request.headers.get("authorization", "")
        if header.startswith("Bearer "):
            return header[7:] == self.token
        return request.q("token") == self.token

    async def handle(self, reader, writer):
        try:
            request = await _read_request(reader)
            if request is None:
                return
            if not self._authorised(request):
                _json(writer, 404, {"error": "not found"})
            elif request.path == "/v1/state":
                await self._state(request, writer)
            elif request.path == "/v1/action":
                await self._action(request, writer)
            elif request.path == "/v1/report":
                await self._report(request, writer)
            elif request.path == "/v1/commands":
                await self._commands(request, writer)
            elif request.path in ("/", "/index.html"):
                self._human(writer)
            elif request.path == "/healthz":
                _json(writer, 200, {"ok": True, "rev": self.hub.rev})
            else:
                _json(writer, 404, {"error": "not found"})
            await writer.drain()
        except (ConnectionResetError, BrokenPipeError, asyncio.IncompleteReadError):
            pass
        except Exception:
            log.exception("request failed")
        finally:
            writer.close()

    # -- device-facing -----------------------------------------------------
    async def _state(self, request: Request, writer) -> None:
        try:
            since = int(request.q("rev") or 0)
        except ValueError:
            since = 0
        try:
            wait = min(MAX_WAIT, float(request.q("wait") or DEFAULT_WAIT))
        except ValueError:
            wait = DEFAULT_WAIT
        if since and wait > 0:
            await self.hub.wait_for_change(since, wait)
        if since and since == self.hub.rev:
            # Nothing moved. 304 keeps the device from redrawing, and keeps the
            # payload off the air entirely.
            _respond(writer, 304)
            return
        _json(writer, 200, self.hub.state())

    async def _action(self, request: Request, writer) -> None:
        if request.method != "POST":
            _json(writer, 405, {"error": "POST only"})
            return
        payload = request.json()
        agent = str(payload.get("agent") or "")
        action = str(payload.get("action") or "")
        if not agent or not action:
            _json(writer, 400, {"error": "agent and action required"})
            return
        local = self.hub.local
        if local is not None and agent in local.agents:
            result = await local.execute(agent, action)
            await self.hub.bump_if_changed()
        else:
            result = self.hub.queue_command(agent, action)
        log.info("action %s -> %s: %s", agent, action, result)
        _json(writer, 200, {"result": result})

    # -- agentd-facing -----------------------------------------------------
    async def _report(self, request: Request, writer) -> None:
        if request.method != "POST":
            _json(writer, 405, {"error": "POST only"})
            return
        payload = request.json()
        machine = str(payload.get("machine") or "")
        if not machine:
            _json(writer, 400, {"error": "machine required"})
            return
        records = []
        now = time.time()
        for item in payload.get("agents") or []:
            try:
                record = AgentRecord(
                    id=item["id"], machine=machine,
                    title=item.get("t", ""), project=item.get("p", ""),
                    branch=item.get("b", ""), activity=item.get("a", ""),
                    question=item.get("q", ""), status=item.get("s", "unknown"),
                    since=now - float(item.get("age", 0)),
                    tokens=int(item.get("tk", 0)),
                    cost_milli=int(item.get("c", 0)),
                    focused=bool(item.get("f")),
                    actionable=bool(item.get("w")),
                    actions=[ActionSpec(id=a[0], label=a[1])
                             for a in item.get("ax", [])],
                )
                diff = item.get("dl") or [0, 0]
                record.lines_added, record.lines_removed = diff[0], diff[1]
            except (KeyError, TypeError, ValueError, IndexError):
                continue
            records.append(record)
        self.hub.remote[machine] = records
        self.hub.remote_seen[machine] = now
        await self.hub.bump_if_changed()
        _json(writer, 200, {"ok": True})

    async def _commands(self, request: Request, writer) -> None:
        machine = request.q("machine")
        if not machine:
            _json(writer, 400, {"error": "machine required"})
            return
        try:
            wait = min(MAX_WAIT, float(request.q("wait") or DEFAULT_WAIT))
        except ValueError:
            wait = DEFAULT_WAIT
        commands = await self.hub.take_commands(machine, wait)
        _json(writer, 200, {"commands": commands})

    # -- humans ------------------------------------------------------------
    def _human(self, writer) -> None:
        state = self.hub.state()
        rows = []
        for a in state["agents"]:
            rows.append(
                f"  {a['s']:<8} {a.get('age', 0):>6}s  {a['m']:<10} "
                f"{a.get('p', ''):<24} {a.get('t', '')[:52]}"
            )
            if a.get("q"):
                rows.append(f"           ? {a['q']}")
            if a.get("ax"):
                rows.append("           " +
                            "  ".join(f"[{i}] {l}" for i, l in a["ax"]))
        body = (
            f"BEACON hub  rev={state['rev']}  "
            f"{state['hh']:02d}:{state['mm']:02d}  {state['d']}\n"
            f"working={state['n']['w']} blocked={state['n']['b']} "
            f"done={state['n']['d']} idle={state['n']['i']} "
            f"machines={state['n']['m']}\n\n" + "\n".join(rows) + "\n"
        )
        _respond(writer, 200, body.encode(), "text/plain; charset=utf-8")

    async def serve(self) -> None:
        server = await asyncio.start_server(self.handle, self.host, self.port)
        addrs = ", ".join(str(s.getsockname()) for s in server.sockets)
        log.info("hub listening on %s", addrs)
        async with server:
            await server.serve_forever()
