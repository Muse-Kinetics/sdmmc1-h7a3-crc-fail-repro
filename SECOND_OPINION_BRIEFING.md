# SDMMC 4-bit data-corruption bug — briefing for independent review

**Historical — served its purpose (a one-time second-opinion review dispatched
2026-09-25/26), superseded by `FINDINGS.md`'s "Round 5"/"Round 6" conclusion
(board-layout/bare-wire-adapter issue, not silicon/driver; the CMD-line
finding this briefing treats as evidence was later retracted). Kept for
record, not maintained going forward — read `FINDINGS.md` for current state.**

This is a condensed handoff, not the full investigation log. Full detail,
if you want to verify a specific claim or dig into raw data, lives in
`FINDINGS.md` in this same directory (and `Bringup/H723ZG/sd-repro/FINDINGS.md`
for the cross-chip work) — but you shouldn't need it to form an opinion.
This doc deliberately leaves out dead ends, tooling war stories, and
firmware bugs we hit and fixed along the way that turned out to be
unrelated to the actual question.

## The symptom

On STM32H7-family MCUs with the "SDMMC v2" peripheral (internal IDMA,
`CMDTRANS`/`CMDSTOP` command bits — as opposed to the older "SDIO"/v1
peripheral on F1/F2/F4/F7/L1/L4-classic parts), a standard 4-bit-wide SD
card read (`HAL_SD_ReadBlocks()`, single or multi-block, polling or
interrupt-driven) reliably fails with `SDMMC_ERROR_DATA_CRC_FAIL`
(`DCRCFAIL` flag set in the peripheral's `STA` register). **1-bit-wide
reads on the identical hardware, pins, and card always succeed.** This
is 100% reproducible, not intermittent.

## Confirmed reproduction matrix

- **Two different chips**, both SDMMC v2/IDMA generation: STM32H7A3ZI
  (NUCLEO-H7A3ZI-Q) and STM32H723ZG (NUCLEO-H723ZG).
- **Four independent software stacks**: our own CubeMX-generated HAL
  code; a manual re-implementation of ST's own BSP reference pattern; a
  completely independent RTOS (Zephyr, its own `drivers/disk/sdmmc_stm32.c`,
  interrupt-driven `HAL_SD_ReadBlocks_IT`, its own clock source); and a
  from-scratch bare-metal repro isolating just the register-level
  read loop.
- **Multiple SD cards**, multiple physical wiring harnesses (Nucleo
  breakout adapters, and — see below — a soldered bare-wire harness).
- **A classic SDIO/v1 peripheral (STM32F401, NUCLEO-F401RE) works fine
  in 4-bit mode**, on cheap bare-wire hardware, up to 12MHz — it only
  breaks (cleanly, deterministically) above that speed, which reads as
  ordinary signal-integrity margin, not a protocol/logic problem.

## Ruled out (each independently tested, none explained the failure)

- Bus width/speed/clock-edge/hardware-flow-control config permutations
- GPIO drive strength (`GPIO_SPEED_FREQ_MEDIUM` vs `VERY_HIGH`) — no effect
- RX sampling-phase delay-block (`DLYB_SDMMC1`) sweep — no clean point found
- IDMA vs polling data-movement mode
- ST's own suggested fix (1-bit-then-switch `BusWide` sequencing) and the
  ST-confirmed community-forum fix (raw `SDMMC_Init()` re-init step)
- HAL/LL version differences (checked a specific real code diff between
  HAL versions that a community thread flagged — not applicable to our
  call path)
- The SD card itself (two different cards, same result; one verified
  byte-for-byte correct when read directly on a host PC)
- **Wiring/harness quality specifically** — see below, this is the most
  recently and most rigorously ruled out
- **Pin mapping/wiring integrity after a physical harness move** —
  independently confirmed via direct logic-analyzer decode (tried all 24
  possible D0-D3 wire orderings against known-good data; the correct
  order wins by a wide, unambiguous margin)

## The two most decisive findings (both new, both from this week)

### 1. Same physical wires: clean on the old peripheral, broken on every speed on the new one

Built a bare soldered microSD-adapter harness on the F401 (classic
SDIO/v1), proved it clean in 4-bit up to 12MHz (hard signal-integrity
wall above that — a normal, expected shape for bare wire). Moved that
*exact physical harness* (no rewiring — pin group is identical between
the two boards) to the H723ZG (SDMMC v2). Result: **100% `DCRCFAIL` at
every tested speed from 400kHz up to 48MHz (no-divide/max speed)** —
including the SD spec's own most conservative identification-mode speed.
The same harness's 1-bit path on the H723ZG, meanwhile, is clean at
every speed including max.

This is a clean natural experiment: the only variable that changed is
the silicon. A wiring/signal-integrity explanation predicts a
speed-dependent failure curve (like the F401 showed); instead there's a
deterministic 0%-vs-100% cliff between 1-bit and 4-bit, independent of
clock speed, on hardware already proven electrically sound at any speed
the silicon can generate.

### 2. Logic-analyzer capture: real corruption on the wire, including on the command line itself

Captured CMD/CLK/D0-D3 during a failing 4-bit read (H723ZG, harness
above, 400kHz). Two things found, independently reproduced across two
separate capture sessions (different reset cycles):

- **Data block**: reconstructing the 512-byte read and diffing against
  known-good ground truth (verified independently, direct host read of
  the card) shows corruption concentrated in the first ~22% of the block
  (bytes 0-111), with the remaining ~78% (including the block's closing
  signature bytes) matching exactly. **This exact shape reproduced
  byte-for-byte identical across two independent captures/resets** —
  fully deterministic, not noise.
- **Command line**: in one capture, the `READ_SINGLE_BLOCK` command
  frame itself (the command the *host* sends to start the read) decoded
  with a single flipped bit in its command-index field — verified via
  the frame's own CRC7 checksum, which matches exactly what a
  *corrected* (un-flipped) frame would produce. Every other bit in the
  40 CRC-protected bits is correct. **This command frame is entirely
  host-driven** (the MCU drives CMD outbound for a command; the card
  only drives it during the response that follows) — so this isn't "a
  signal arrived corrupted and got misread," it's the MCU's own
  peripheral producing a wrong bit on a line it is actively driving.
  In a second capture (independent reset), a command-line anomaly was
  present again but with a different, less clean pattern — i.e. **the
  command-line effect is not deterministic run-to-run, unlike the data
  corruption, which is.**

## Our current working theory (ours — please form your own independent view, don't just evaluate ours)

The host-driven nature of the CMD-line bit flip argues against ordinary
external electrical noise/crosstalk (a strongly-driven push-pull output
is hard to disturb from outside, especially at 400kHz, where classical
signal-integrity effects like reflections/ringing/crosstalk should be
negligible — bit periods are 2.5µs, far longer than any transient on a
few-inch wire). We currently favor something internal to the SDMMC v2
peripheral's own command/data state-machine sequencing right at the
moment it transitions into a 4-bit data phase — a timing/sequencing
hazard in the silicon or in how the transition is driven, rather than a
config mistake in our code (we've tried the config levers we could find;
see "ruled out" above). The data-line corruption could share that root
cause, or have a separate, more classical crosstalk-style explanation
(D0-D3 during a read are card-driven and weakly pulled, more plausible
targets for coupling from a neighbor). We have not resolved which.

## An additional, unverified lead (separate data point — not vetted through our normal process)

A secondary analysis pass — one we didn't fully control the process or
context for, and can't vouch for the independence of — surfaced a
specific, different theory. We're including it as one input to weigh,
*not* as an established finding, because its single most checkable
prediction held up when we independently re-verified it ourselves
against our own committed ground-truth data (not trusting the
self-reported check).

**The claim:** every dropped nibble observed corresponds to a
`0x0`→`0xF` nibble transition — the moment all four D-lines
simultaneously switch from all-low to all-high, the largest possible
simultaneous-switching event this bus can produce. It predicted this at
four specific byte positions in one capture; independently checking all
four against the ground-truth bytes already in this repo's
`captures/decode_capture.py`, all four land exactly on a `0`→`F`
transition.

**The proposed mechanism:** this simultaneous switching event induces a
spurious extra clock edge at the *card* (crosstalk/ground-bounce), so
the card's own bit counter skips a nibble — once per occurrence of this
specific transition. This would be content-dependent (corruption
clusters wherever a given sector's content happens to contain `0`→`F`
transitions, rather than "the start of every transfer" as we'd been
assuming) and clock-speed-independent (consistent with our "fails at
every speed" result). It also separately challenges the CMD-line finding
above: it argues that CLK and CMD transitions can land within the same
analyzer sample interval given how soon after a CLK edge SDMMC v2
launches its next bit, so the "single flipped bit" we found might be a
decode-side timing-ambiguity artifact rather than a genuine corruption —
while reporting that the card's real R1 response to that same command
decoded cleanly.

**Treat this as one specific, partially-verified hypothesis to weigh
alongside your own analysis, not as a second opinion already delivered.**
We do not know what context or framing it was given, so we can't assess
its independence the way we can for the rest of this document. If useful,
`captures/f401-harness/` and `captures/` (see appendix) have the raw data
and ground truth to check the `0x0→0xF` correlation yourself — including
against corrupted regions beyond the four positions already spot-checked,
and against other transition types, to see whether the pattern actually
discriminates or just happened to match here.

## What we'd like from a fresh, independent pass

We specifically want your own independent reasoning here — not a
verdict on the lead above. Please generate your own hypotheses first,
using the evidence in this document, before weighing in on it.

1. What's your own best explanation for the evidence above, independent
   of both our "peripheral-internal sequencing hazard" theory and the
   `0x0→0xF` lead? We'd rather have two or three genuinely distinct
   candidate mechanisms from you than a single verdict on one theory.
2. Does the evidence support a driver/config-fixable explanation at all,
   or does it point toward something we can't fix in software (a
   hardware erratum, a fundamental limitation of bare-wire wiring with
   this peripheral generation, etc.)?
3. Is there a known STM32H7 SDMMC errata, published or informally
   documented, matching this shape (4-bit-only failure, 1-bit clean,
   corruption concentrated in specific parts of a transfer)?
4. Any other public reports (forums, GitHub issues, other RTOS/HAL
   projects) describing a signature like ours — front-of-transfer
   corruption, content-dependent dropouts, and/or command-line corruption
   on a host-driven frame during a 4-bit transaction — that we haven't
   found?
5. What's the highest-value next experiment, in your view, given
   everything above — including but not limited to testing the
   `0x0→0xF` theory? (We have a Saleae logic analyzer, full
   register-level firmware control, and can build custom test firmware
   on either chip family quickly. A second SD-host peripheral generation
   check — e.g. an STM32H5 or STM32U5's SDMMC — is also within reach if
   that would help isolate "SDMMC v2 specifically" vs. "this exact H7
   die.")

## Appendix: where everything lives, and how to navigate it

Everything below is under `Reference/` in the project repo. You should
not need to read most of it — this is here so that if you want to verify
a specific claim, see raw data, or check whether something's already
been tried, you know exactly where to look instead of searching broadly.

### `Bringup/st-sd/st-sd-ref/` — the main hub for this investigation

- **`FINDINGS.md`** — the full, currently-accurate investigation log.
  Long (~1100 lines), organized with `## `/`### ` headers you can jump
  to; a github-style renderer or `grep -n '^## '` gives a table of
  contents. This is the primary source if you want more than this
  briefing gives you.
- **`README.md`** — the short version: problem statement, repro steps,
  pinout, compact "what we tried" summary. This is what shipped to ST
  support alongside the public repro repo (see below) — written to read
  like a normal bug report, not a wall of diagnostics.
- **`corrections.md`** — mistakes, overclaims, and dead-end detours from
  this investigation, kept separate from `FINDINGS.md` specifically so
  the main log stays a record of current understanding. Worth a skim if
  you want to avoid re-treading a path already found unproductive.
- **`captures/`** — logic-analyzer data and the decode tooling:
  - `decode_capture.py` + `read_lba8192_clockdiv40.csv` — the original
    H7A3 capture and its decoder (validates the SD command stream via
    CRC7, reconstructs the data block, diffs against known-good content).
  - `f401-harness/` — the F401-harness-on-H723ZG captures (two, from
    independent reset cycles) and a copy of the decoder pre-loaded with
    this specific card's ground-truth bytes, plus a per-lane D0-D3
    ordering sanity check. `README.md` in that folder explains which
    file is which.
  - Both decoder scripts are dependency-free Python (stdlib only) — run
    directly, no environment setup needed, if you want to inspect a
    capture yourself or write a variant analysis.
- **`THIRD_PARTY_LICENSES.md`** — not relevant to the bug; licensing
  paperwork for vendored ST/ChaN code in this repo, present because this
  directory is also the public repro repo (see below).

### `Bringup/H723ZG/sd-repro/` — the H723ZG cross-chip repro project

A parallel CubeMX project targeting NUCLEO-H723ZG instead of the H7A3,
used for every cross-chip test in this investigation (the Zephyr RTOS
work, and the Round 4 known-good-harness test). Its own `FINDINGS.md`
has H723ZG-specific detail and links back to the main `st-sd-ref`
`FINDINGS.md` for anything that isn't specific to this board.

### `Bringup/F401-known-good/sdio-test/` — the classic-SDIO baseline

A from-scratch project for NUCLEO-F401RE (the older "SDIO"/v1
peripheral, not SDMMC v2). This is where the known-good bare-wire
harness was built and proven clean in 4-bit mode up to 12MHz, before
being moved (unmodified) to the H723ZG for the Round 4 comparison.
`README.md` there has the harness construction details and clock-sweep
result.

### `Bringup/WeAct-H750/sd-test/` — a third board, prepared but not yet run

A project for a WeAct "MiniSTM32H7xx" board (STM32H750, also SDMMC v2,
but a purpose-built PCB with real matched-length routing and pull-ups —
unlike every Nucleo-breakout-adapter test elsewhere in this
investigation). Firmware is built and ready; the board itself hadn't
arrived as of this briefing. If it becomes available, a clean result
here would point at ground-plane/routing quality as a variable; a
failure on proper PCB routing would be further evidence against a
wiring explanation entirely.

### External

- `https://github.com/Muse-Kinetics/sdmmc1-h7a3-crc-fail-repro` — the
  public repro repo filed with ST support (this *is*
  `Bringup/st-sd/st-sd-ref/`, mirrored — same content).
- ST support ticket **00268848** — filed against the original H7A3
  finding; the cross-chip/Saleae evidence gathered since filing is
  stronger than what ST has seen so far and hasn't been re-reported yet.
