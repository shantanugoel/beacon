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

---

## 7. What the simulator taught us (before any hardware)

**Dither is for areas, never for type.** The very first render used the ink
scale for secondary text — 25% grey for row metadata, 50% for footer hints. On
screen it looked like a smudge: at 9–12 px an ordered-dither pattern eats more
pixels than the letterforms have. Every string on every screen is now drawn at
full ink or full paper, and hierarchy is carried entirely by size, weight and
space. The tonal scale survives, but only for fills, bands and rules.

**Do not say the same thing twice on a 400x300 panel.** The promoted "needs
you" agent was also appearing as the first row of the fleet list. Cutting the
duplicate bought back a row *and* made the band read as a distinct object
rather than a restatement.

**A black slab under a black header is one mass.** Selecting the attention
band originally inverted it. Directly below the solid header that turned the
top half of the screen into a single block — and on e-ink you pay for black
pixels twice, in refresh time and in ghosting. Selection is now shown by
border weight plus a caret; full inversion is reserved for list rows, where it
is surrounded by white.

**Bottom-anchored blocks collide.** The session screen originally pinned the
action list to the bottom and let the content grow down into it. On a fixed
300 px panel that is a collision waiting to happen. It now flows strictly
top-down against an explicit budget, and when the page runs out of room it is
the *least* important block (the stat row) that gets dropped, never the
actions.

**9 px is below IBM Plex Mono's floor.** At 9 px the capital M loses its
middle vertex entirely. The label face moved to 10 px.

**Stepping angles does not draw a cone.** The ambient screen's beam started as
a fan of rays at fixed angular steps; as radius grows those rays separate and
the result reads as moiré, not light. It is now evaluated per pixel — about
120k integer operations, which is nothing next to the refresh that follows.

**Hashing alone is not a layout.** Placing each session at a hash-derived
angle let two sessions on the same ring land on top of each other, which made
a calm fleet look like a broken one. Position is now an even spread by index
with a per-id jitter.

---

## 8. Hardware measurements

Measured on the device via `beacon-preview`, a console command that draws every
screen and times each refresh mode. Times are the wall clock around the
driver's synchronous BUSY handshake.

| Refresh | Region | Measured |
| --- | --- | --- |
| Full 1bpp | 400 x 300 | **1121 ms** |
| Partial 1bpp | 40 x 110 (3%) | **758 ms** |
| Partial 1bpp | 384 x 284 (90%) | ~760 ms |
| Full 4bpp (16-grey) | 400 x 300 | **8211 ms** (incl. the mandatory white 1bpp flush) |

Two results reshaped the design:

**Partial refresh is not much faster than full, and its cost does not scale
with area.** 758 ms for 3% of the panel and about the same for 90% of it: the
SSD2683's partial waveform is a fixed-length sequence. So partial refresh is
not a performance optimisation at all — its entire value is that it *does not
flash*. That reframes the diffing logic in `Display::Present`: it is there to
keep the screen calm, not to make it quick. It also means there is no reason
to chase a tighter dirty rectangle.

**Elapsed times must be shown at minute resolution.** A refresh costs ~760 ms,
so the panel updates about once a minute. Rendering "4m12s" would put a
seconds field on screen that is stale almost all the time. Durations above a
minute are now shown to the minute; below a minute, where a fresh block really
does want seconds, they are exact. The display no longer claims precision it
cannot honour.

**16-grey costs eight seconds**, and the device cannot service buttons while it
runs. That is affordable precisely because the ambient screen is entered only
after several minutes of inactivity and left by a fast 1bpp refresh — but it
confirms that spending greyscale anywhere in the interactive path would have
been a mistake.

Rendering the whole frame and diffing it costs about a millisecond of CPU;
next to 758 ms of panel time it does not appear in the measurements. Keeping
per-widget invalidation out of the screen code was free.

---

## 9. Bugs worth remembering

**A `Fleet` is 8.5 KB, and the main task stack was 8 KB.** Copying one into a
local — `const Fleet saved = g_fleet;` — overflowed the stack and corrupted the
heap. It surfaced as a `LoadProhibited` panic inside `spi_bus_remove_device`,
three layers down in the display driver, which is about as misleading as a
backtrace gets. Every `Fleet` now lives in static storage, there is a
`static_assert` saying why, and the same latent bug was present in the normal
network path, not just the preview.

**The provisioning console was unreachable over the only cable the device
has.** With ESP-IDF's default console configuration, logs are mirrored to
USB-Serial/JTAG but *input* is routed to UART0 on GPIO43/44. Serial writes
simply blocked forever. `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` makes USB the
primary console, which is the only sane setting for this board.

**Internal RAM is the scarce resource, not flash.** The 120 KB greyscale
scratch surface and three `Fleet` snapshots overflowed DRAM by 16 KB while the
app partition sat 74% empty. They are now placed in PSRAM via
`EXT_RAM_BSS_ATTR` (`beacon_mem.h`). The 1bpp canvas deliberately stays in
internal RAM: it is a SPI DMA source and the hot path for every draw call.

**herdr's per-pane `focused` flag does not mean "the pane you are looking
at".** It means "focused within its own tab", so on a multi-workspace setup
almost every pane claims it. The focus caret uses `pane.current` instead.

**Toolchain, for the next person:** this machine's ESP-IDF came from the
ESP-IDF Installation Manager. The checkout's own `export.sh` fails (it looks
for a venv under `~/.espressif/python_env`); the entry point is
`~/.espressif/tools/activate_idf_v6.1.sh`, which additionally refuses to be
sourced from a script and exposes `idf.py` only as a shell function. The
supported non-interactive path is `activate_idf_v6.1.sh -e`, which prints the
environment — see `tools/idf.sh`.

---

## 10. Bringing the radio up (and a misleading crash)

Once real credentials went in, the device rebooted in a loop. The panic was
`StoreProhibited` inside the **Wi-Fi driver's own power-management timer path**:

```
timer_remove -> esp_timer_stop -> ets_timer_disarm
  -> esp_coex_common_timer_disarm_wrapper
  -> pm_enable_active_timer -> pm_rx_data_process -> ppRxPkt -> ppTask
```

Nothing in that stack is BEACON code, which made it look like a driver or a
configuration problem. Two plausible suspects were changed at once to get the
device usable — modem sleep (`WIFI_PS_MIN_MODEM`, and the crash was squarely
in the power-management path) and PSRAM-resident `.bss`
(`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`, which had just been added).

That got past the loop and straight into the actual bug, which the next boot
named outright:

```
***ERROR*** A stack overflow in task beacon_net has been detected.
```

`Net::ParseState` was building its result in a local `Fleet next{}` — the same
8.5 KB-on-the-stack mistake already fixed once in `app_main`, missed here
because it looked like an ordinary local. It overflowed the 8 KB network task
stack and corrupted whatever sat next to it, which happened to be Wi-Fi driver
state; hence a crash in `ppTask` with no BEACON frame anywhere in the
backtrace.

The fix is a `staging_` member that a response is parsed into and only copied
into the live fleet once the whole payload parses — so a truncated response
can never leave a half-updated screen either. The task stack went to 12 KB for
margin.

**Then both suspects were re-tested rather than left convicted.** Modem sleep
was re-enabled and ran clean, so it was innocent and the battery win is kept.
The PSRAM `.bss` change was *not* reverted wholesale — the big buffers stay in
PSRAM, but now through `heap_caps_malloc(MALLOC_CAP_SPIRAM)` at runtime
(`beacon_mem.h`) rather than by placing the BSS segment there. That is the
pattern ESP-IDF supports alongside Wi-Fi, and it leaves the segment layout
alone.

The lesson worth keeping: **a backtrace with none of your own frames in it is
often still your bug.** A stack overflow lands the blame wherever the
neighbouring memory happens to live.

---

## 11. Verified on hardware

| | |
| --- | --- |
| Boot, panel, all five screens | `beacon-preview` draws each and logs its refresh time |
| Provisioning over USB console | `beacon-set` / `beacon-save`, credentials survive reflash |
| Wi-Fi association | `192.168.1.20`, same /24 as the hub, ~2.6 s from boot |
| Hub long-poll | revisions arriving; `304` on no change |
| Refresh behaviour in steady state | one full refresh on first paint, then flash-free partials of 750–765 ms |
| Attention path | a blocked agent injected at the hub sorted to the top and triggered a redraw of the band within a second |
| Action path | option fired from the device reached the owning machine's command queue; `focus` executed against herdr for real |
| Stale-option guard | an option the agent is no longer offering is refused rather than sent |
| Alert | ES8311 comes up and the chirp plays; `beacon-alert` fires it on demand |

The refresh log from a steady-state minute is the clearest evidence the design
works as intended:

```
beacon.net: rev 70: 2 agents (1 working, 1 blocked)
beacon.disp: full 384x153 (48%) in 1124 ms      <- attention band appears
beacon.disp: partial 384x131 (41%) in 818 ms    <- band clears
beacon.disp: partial 96x8 (0%) in 752 ms        <- a timer ticks over
```

The 48% change crossed the full-refresh threshold and correctly took the
flash; the 41% one stayed under it and did not.

---

## 12. Left undone

- **The ambient screen blocks input for its 8.2 s refresh.** Entering quiet
  mode makes the device deaf to buttons for the duration. It needs several
  idle minutes to trigger, so it is rare, but a button press in that window is
  simply lost. Fixing it properly means moving the panel onto its own task
  with a cancellable refresh.
- **Elapsed times are seeded, not known.** On a cold start the collector has
  no record of how long a session has held its state; it uses the transcript's
  last-write time as a lower bound. A collector that persisted state across
  restarts would be exact.
- **The transcript-only fallback can never report "blocked"**, because a
  permission prompt is not written to the transcript. On a machine without
  herdr, BEACON is an ambient display rather than a pager. Claude Code's
  `Notification` hook would close that gap.
- **No TLS.** The hub speaks plain HTTP with an optional bearer token. That is
  a deliberate fit for a LAN device, and the wrong choice the moment the hub
  is exposed beyond one.
- **NFC is unused.** The board has an ST25-class tag at 0x55; writing the hub
  URL and credentials to it would make provisioning a phone tap instead of a
  serial console.
