# BEACON — research & design log

A running log of what I found, what I decided, and what I traded away.
Newest entries at the bottom of each section.

---

## 1. Hardware reconnaissance

**Device.** Confirmed over `/dev/ttyACM0` with esptool:

```
Chip type:  ESP32-S3 (QFN56) rev v0.2
Features:   Wi-Fi, BT5 LE, dual core + LP core, 240 MHz, 8 MB embedded PSRAM (AP_3v3)
Flash:      16 MB
MAC:        (omitted)
USB:        USB-Serial/JTAG (native, no CP210x)
```

`/dev/ttyACM0` is `root:uucp 660` and the user is in `uucp`, so flashing needs no sudo.

**Panel.** SSD2683, 400 x 300, 4.2". That works out to ~119 ppi
(3.36" x 2.52" active area) — high enough that 12–14 px text is genuinely
readable, which is what makes an information-dense layout possible at all.

Three refresh modes exposed by the vendor driver:

| Mode | Buffer | Notes |
| --- | --- | --- |
| Full 1bpp | 15,000 B | Flashing full-panel update, establishes the base image |
| Partial 1bpp | `ceil(w/8)*h` | No flash, needs a prior full 1bpp base, accumulates ghosting |
| Full 4bpp | 60,000 B | 16 grey levels, slow, kills the partial-refresh base |

The last row is the important constraint: **4bpp and partial refresh are
mutually exclusive.** After a greyscale frame you must lay down a fresh full
1bpp base before any partial update. That single fact shaped the whole UI —
see §4.

**Peripherals** (from `zectrix_board_config.h`): 3 buttons (OK/GPIO0,
UP/GPIO39, DOWN/GPIO18 — DOWN is also the power button with a 3 s shutdown
hold), power LED GPIO3, battery latch GPIO17, PCF8563 RTC at 0x51, NFC at
0x55, ES8311 codec + speaker PA, charge detect/full on GPIO2/GPIO1, battery
voltage on an ADC path.

**Toolchain.** Only ESP-IDF **v6.1** is installed
(`~/.espressif/v6.1/esp-idf`). The vendor demo README says 5.4+, but the
demo's `build/` directory shows it was already configured and flashed against
6.1, so the driver compiles clean on 6.1. Good — no toolchain archaeology.

---

## 2. Where the data actually comes from

The brief says "AI agents and command sessions I run across multiple
machines". Before inventing a protocol I went looking for what already knows
about those sessions on this machine.

**Claude Code transcripts.** `~/.claude/projects/<slug>/<session>.jsonl`.
Rich: `ai-title` (a human-readable session title), `cwd`, `gitBranch`,
`cost-state` (cost, lines added/removed, durations), plus every assistant and
tool message. Everything needed to describe a session — but *state* has to be
inferred from the tail of the file, and "is it waiting on a permission
prompt?" is a guess based on a `tool_use` with no matching result.

**herdr.** The user already runs `herdr` (a terminal workspace manager for
coding agents) — its `SessionStart` hook is installed in
`~/.claude/settings.json`, and the server was live during this work. It
exposes a newline-delimited JSON-RPC socket at
`~/.config/herdr/herdr.sock`. Probing it turned up exactly the ontology a
pager wants:

```
AgentStatus = idle | working | blocked | done | unknown
```

and these methods, all verified against the running server:

- `agent.list` — every agent with `agent_status`, `cwd`, `terminal_title`
  (which is the AI-generated session title), `pane_id`, `workspace_id`,
  `focused`
- `agent.read {source: visible|recent|detection}` — the actual pane text, so
  the device can show *what the agent is doing right now*, including the
  spinner line with elapsed time and token count
- `agent.prompt`, `agent.send_keys`, `agent.focus` — **write** access. The
  device can answer a permission prompt, send a prompt, or make the desktop
  jump to a pane
- `events.subscribe` — push. `pane.agent_status_changed` is per-pane
  (requires a `pane_id`), but `pane.updated` / `pane.created` /
  `pane.closed` / `pane.agent_detected` are global and `pane.updated` carries
  the full pane record including `agent_status`. Verified streaming works.

**Decision.** herdr is the primary source: it is push-based, already
correct about the hard part (status detection), and it is the only thing that
gives the device *write* access back to a session. Claude Code transcripts
are a secondary enrichment (cost, lines changed, branch) and the fallback for
machines without herdr. Neither is a hard dependency — the collector degrades
to whichever is present.

This is the difference between a dashboard and a pager: without write access
the device can only tell you something is wrong. With it, the device can fix
it from across the room.

---

## 3. System shape

```
  machine A ──┐
  machine B ──┼── beacon-agentd ──HTTP──▶ beacon-hub ◀──HTTP── NOTE4 firmware
  machine C ──┘   (collector +            (aggregate,          (render + input)
                   actuator)               long-poll)
```

**Why a hub rather than the device polling each machine.** The device has one
radio, a battery, and no DNS-worth of config. Making it talk to N machines
means N connections, N credentials, and N failure modes on the least capable
node in the system. One hub, one URL.

**Why the device renders, and not the hub.** The tempting shortcut is to
render the 400x300 bitmap server-side with Pillow and let the firmware blit
it. Rejected: every button press would become a network round trip, and the
device would show nothing useful when the hub is unreachable. The firmware
owns the UI and the hub sends compact state. Cost of that choice is that I
have to build real typography on-device — see §5 for how that was made cheap.

**Transport.** HTTP long-poll (`GET /v1/state?rev=N`, hub holds the request
open ~25 s and returns on change, else `304`). Push latency without a broker,
and no MQTT dependency. Actions go back as `POST /v1/action`.

**Wire format.** JSON. cJSON ships with ESP-IDF and 8 MB of PSRAM makes the
parse cost irrelevant; a hand-rolled binary TLV would have saved ~2 KB per
poll and cost a day of debugging.

---

## 4. Designing for the panel, not around it

The refresh table in §1 forces a discipline most embedded dashboards ignore.

- **Greyscale is a destination, not a decoration.** A 4bpp frame is slow and
  invalidates the partial-refresh base. So 16-grey is spent only on the
  ambient/quiet screen, which changes a few times an hour. Every interactive
  screen is 1bpp.
- **Tone in 1bpp comes from dithering.** Rather than living in pure black and
  white, the UI has an *ink scale* — 0 / 12.5 / 25 / 50 / 75 / 100 % —
  realised with an ordered Bayer matrix. This is what lets a 1-bit panel have
  a filled header band, recessed rows, and a sense of depth, while still
  updating with a flash-free partial refresh.
- **Refresh budget as a design constraint.** Target is a few updates per
  minute at rest. Live-ish elements (clock, elapsed timers, the working
  spinner) live inside small partial-refresh regions; layout changes trigger
  a full refresh. Every N partial updates, a full refresh is forced to clear
  accumulated ghosting.
- **No animation as ornament.** Motion costs ghosting and power. The one
  moving element is a deliberate one: a slow sweep in the quiet screen.

---

## 5. The host simulator (the decision that made the visuals possible)

Iterating on pixel layout by reflashing is a ~60 s loop and you cannot diff
two screenshots. So the entire drawing stack — canvas, ink/dither, fonts,
icons, and every screen — is plain C++ that depends on nothing but a
framebuffer. It compiles twice: once into the ESP-IDF app, once into a host
binary that writes PNGs.

That turns a design change into a sub-second `make && open` loop, and makes
the layouts reviewable as images in this repo.

---

## 6. Open questions carried into implementation

- Exact wall-clock timing of each refresh mode on real hardware — measured
  on-device, recorded in §8.
- How legible 12 px text really is at 119 ppi through the panel's front
  layer, versus how it looks in the simulator.
- Whether `agent.read` pane text is clean enough to extract a one-line
  "current activity" string reliably.
