"""Parser tests built from text actually captured off live panes."""

import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from beacon.panes import parse_pane

WORKING = """\
● Boots -6 and -4 end on a completed backup with the machine otherwise idle.

  Ran 1 shell command

● Classify every boot as clean or abrupt · 3s
  ⎿  $ for b in $(seq -24 -1); do
     last=$(journalctl -b $b --no-pager -o short-precise 2>/dev/null | tail -1)

✽ Gusting… (4m 14s · ↓ 6.6k tokens)
  ⎿  Tip: Use /statusline to set up a custom status line

────────────────────────────────────────────────
❯
────────────────────────────────────────────────
  ⏵⏵ auto mode on (shift+tab to cycle) · esc to interrupt
"""

PERMISSION = """\
● I'll flash the firmware now.

╭──────────────────────────────────────────────────────────╮
│ Bash command                                             │
│                                                          │
│   idf.py -p /dev/ttyACM0 flash                           │
│   Flash the built firmware to the connected NOTE4        │
│                                                          │
│ Do you want to proceed?                                  │
│ ❯ 1. Yes                                                 │
│   2. Yes, and don't ask again for idf.py commands        │
│   3. No, and tell Claude what to do differently (esc)    │
╰──────────────────────────────────────────────────────────╯
"""

PLAN = """\
● Here is the plan.

 Would you like to proceed?
 ❯ 1. Yes, and auto-accept edits
   2. Yes, and manually approve edits
   3. No, keep planning
"""

IDLE = """\
● Done. Three files changed, tests pass.

✻ Cooked for 3m 13s · done 4:55 PM

────────────────────────────────────────────────
❯
"""


def test_working_activity_and_tokens():
    r = parse_pane(WORKING)
    assert r.activity == "Classify every boot as clean or abrupt", r
    assert r.tokens == 6600, r
    assert r.elapsed == "4m 14s", r
    assert r.options == []


def test_permission_prompt_options_are_read_not_guessed():
    r = parse_pane(PERMISSION)
    assert [o.id for o in r.options] == ["1", "2", "3"], r
    assert r.options[0].label == "Yes"
    assert r.options[1].label.startswith("Yes, and don't ask")
    # The "(esc)" hint is stripped; the label stays readable at 26 chars.
    assert "(esc)" not in r.options[2].label
    assert all(len(o.label) <= 26 for o in r.options)
    # The question leads with the thing under review, not the bare prompt.
    assert "Flash the built firmware" in r.question, r
    assert "Do you want to proceed?" in r.question, r


def test_plan_prompt():
    r = parse_pane(PLAN)
    assert [o.id for o in r.options] == ["1", "2", "3"]
    assert r.question.endswith("Would you like to proceed?"), r


def test_idle_pane_offers_nothing():
    r = parse_pane(IDLE)
    assert r.options == []
    assert r.activity == "Done. Three files changed, tests pass.", r


def test_empty():
    r = parse_pane("")
    assert r.activity == "" and r.options == []


def test_stray_numbers_are_not_an_offer():
    # A numbered list in prose must never become buttons: pressing one would
    # send a real keystroke into a real session.
    r = parse_pane("● Steps:\n  1. build\n  2. flash\n\n✽ Working… (2s)\n")
    assert r.options == [], r


def test_offer_buried_in_scrollback_is_ignored():
    old = PERMISSION + "\n".join(f"● step {i}" for i in range(20)) + "\n"
    assert parse_pane(old).options == []


if __name__ == "__main__":
    import traceback
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  PASS {name}")
            except Exception:
                failures += 1
                print(f"  FAIL {name}")
                traceback.print_exc()
    raise SystemExit(1 if failures else 0)
