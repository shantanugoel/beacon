"""Regression tests for how an agent's action list is built."""

import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from beacon.collector import Collector
from beacon.model import ActionSpec, AgentRecord


def make(status="working"):
    c = Collector(machine="test")
    r = AgentRecord(id="test:w1p1", machine="test", pane_id="w1:p1",
                    status=status)
    return c, r


def test_actions_do_not_accumulate_across_updates():
    # The pane is only re-read every few seconds, but pane events arrive far
    # more often; composing must be idempotent.
    c, r = make("working")
    for _ in range(5):
        c._compose_actions(r)
    ids = [a.id for a in r.actions]
    assert ids == ["interrupt", "focus"], ids


def test_agent_offered_options_come_first_and_survive():
    c, r = make("blocked")
    r.offered = [ActionSpec(id="1", label="Yes"),
                 ActionSpec(id="2", label="No")]
    c._compose_actions(r)
    c._compose_actions(r)
    ids = [a.id for a in r.actions]
    assert ids == ["1", "2", "focus"], ids


def test_capped_at_the_devices_array_size():
    c, r = make("blocked")
    r.offered = [ActionSpec(id=str(i), label=f"Option {i}") for i in range(1, 7)]
    c._compose_actions(r)
    assert len(r.actions) == 4, r.actions
    # The agent's own options win the space over the convenience ones.
    assert [a.id for a in r.actions] == ["1", "2", "3", "4"]


def test_idle_agent_offers_continue():
    c, r = make("idle")
    c._compose_actions(r)
    assert [a.id for a in r.actions] == ["continue", "focus"]


if __name__ == "__main__":
    import traceback
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn(); print(f"  PASS {name}")
            except Exception:
                failures += 1; print(f"  FAIL {name}"); traceback.print_exc()
    raise SystemExit(1 if failures else 0)
