"""Entry points.

  python -m beacon hub          run the hub (with a local collector by default)
  python -m beacon agentd       run a collector that reports to a remote hub
  python -m beacon watch        print the local fleet to a terminal, no server
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import sys
import time
import urllib.error
import urllib.request

from .collector import Collector, machine_name
from .hub import Hub
from .server import Server


def _log(verbose: bool) -> None:
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(asctime)s %(levelname)-5s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )


async def run_hub(args) -> None:
    local = None if args.no_local else Collector(herdr_socket=args.herdr_socket)
    hub = Hub(local=local)
    server = Server(hub, host=args.host, port=args.port, token=args.token)
    tasks = [hub.ticker(), server.serve()]
    if local is not None:
        tasks.append(local.run())
    await asyncio.gather(*tasks)


async def run_agentd(args) -> None:
    """Collect locally, push to a remote hub, and long-poll it for actions.

    The agent machine never listens on a port: it reaches out for work the same
    way the device reaches out for state. One firewall hole, at the hub.
    """
    collector = Collector(machine=args.machine, herdr_socket=args.herdr_socket)
    base = args.hub.rstrip("/")
    headers = {"Content-Type": "application/json"}
    if args.token:
        headers["Authorization"] = f"Bearer {args.token}"
    log = logging.getLogger("beacon.agentd")

    def post(path: str, payload: dict, timeout: float) -> dict:
        request = urllib.request.Request(
            base + path, data=json.dumps(payload).encode(),
            headers=headers, method="POST")
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read() or b"{}")

    def get(path: str, timeout: float) -> dict:
        request = urllib.request.Request(base + path, headers=headers)
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read() or b"{}")

    async def report_loop():
        while True:
            now = time.time()
            payload = {
                "machine": collector.machine,
                "agents": [r.wire(now) for r in collector.snapshot()],
            }
            try:
                await asyncio.to_thread(post, "/v1/report", payload, 10)
            except (urllib.error.URLError, OSError, TimeoutError) as exc:
                log.warning("report failed: %s", exc)
                await asyncio.sleep(5)
                continue
            # Push on change, but never go longer than 20 s without a
            # heartbeat, or the hub will time this machine out.
            await collector.wait_dirty(20.0)

    async def command_loop():
        path = f"/v1/commands?machine={collector.machine}&wait=25"
        while True:
            try:
                payload = await asyncio.to_thread(get, path, 40)
            except (urllib.error.URLError, OSError, TimeoutError) as exc:
                log.warning("command poll failed: %s", exc)
                await asyncio.sleep(5)
                continue
            for command in payload.get("commands") or []:
                result = await collector.execute(command["agent"],
                                                 command["action"])
                log.info("executed %s %s -> %s", command["agent"],
                         command["action"], result)

    await asyncio.gather(collector.run(), report_loop(), command_loop())


async def run_watch(args) -> None:
    collector = Collector(herdr_socket=args.herdr_socket)
    task = asyncio.create_task(collector.run())
    try:
        deadline = time.time() + args.seconds if args.seconds else None
        while deadline is None or time.time() < deadline:
            await collector.wait_dirty(3.0)
            records = sorted(collector.snapshot(),
                             key=lambda r: (r.status != "blocked", r.id))
            print(f"\n--- {time.strftime('%H:%M:%S')} "
                  f"({'herdr' if collector.using_herdr else 'transcripts'}) ---")
            for r in records:
                print(f"  {r.status:<8} {int(time.time()-r.since):>5}s "
                      f"{r.project[:22]:<22} {r.title[:46]}")
                if r.activity:
                    print(f"           · {r.activity}")
                if r.question:
                    print(f"           ? {r.question}")
                if r.actions:
                    print("           " + "  ".join(
                        f"[{a.id}] {a.label}" for a in r.actions))
    finally:
        task.cancel()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="beacon")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--herdr-socket", default=None)
    sub = parser.add_subparsers(dest="cmd", required=True)

    hub = sub.add_parser("hub", help="run the hub the device talks to")
    hub.add_argument("--host", default="0.0.0.0")
    hub.add_argument("--port", type=int, default=8787)
    hub.add_argument("--token", default=None)
    hub.add_argument("--no-local", action="store_true",
                     help="aggregate only remote agentd reports")
    hub.set_defaults(run=run_hub)

    agentd = sub.add_parser("agentd", help="report this machine to a hub")
    agentd.add_argument("--hub", required=True, help="http://host:8787")
    agentd.add_argument("--token", default=None)
    agentd.add_argument("--machine", default=machine_name())
    agentd.set_defaults(run=run_agentd)

    watch = sub.add_parser("watch", help="print the local fleet and exit")
    watch.add_argument("--seconds", type=float, default=0)
    watch.set_defaults(run=run_watch)

    args = parser.parse_args(argv)
    _log(args.verbose)
    try:
        asyncio.run(args.run(args))
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
