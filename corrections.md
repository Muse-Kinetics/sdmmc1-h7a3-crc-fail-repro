# Corrections — dead ends, wrong turns, and mistakes in this investigation

This is a companion to `FINDINGS.md`, not a replacement for it. `FINDINGS.md`
documents what we know; this documents what we got wrong or wasted effort on
along the way, so a future pass doesn't repeat it. Ordinary ruled-out
config levers (clock speed, bus width, GPIO drive strength, etc.) are NOT
here — those are real findings and belong in `FINDINGS.md`'s "ruled out"
sections. This file is specifically for reasoning errors, premature
conclusions, and detours that turned out to be wrong or unhelpful.

## Chased a signal-conditioning/level-shifter theory that our own evidence already contradicted

While researching why ST's own H7 eval boards (which have SD slots) might
handle 4-bit differently, found a Nexperia IP4856CX25 level-translator/ESD IC
on ST's H753-EVAL schematic and built a theory that ST's reference designs
"always" buffer the SD bus, implying our bare-wire setups were missing
something ST considers mandatory. This was wrong, and the evidence against
it was already in hand before the theory was built: STM32World's
STM32F405 4-bit SDIO example (bare wire, no level-shifter) and betaflight's
production SDIO driver (also bare wire, millions of real units) both work
fine in 4-bit without any such IC. Corrected directly by the user. Lesson:
weigh existing evidence before building a new theory on top of a single new
data point.

## Diagnosed a Zephyr "board hang" that was a tooling quirk, and built a fix for it before checking

Early in the Zephyr RTOS work (`Bringup/H723ZG/sd-repro/FINDINGS.md`,
"Round 3"), a plain SWD reconnect failing after flashing `hello_world` was
read as a boot hang, attributed to the board's stock 550MHz PLL not
locking. Built and flashed an alternate PLL config as a fix attempt before
verifying the actual failure — only via GDB (`-k`, connect-under-reset,
breakpoint at `main()`) was it confirmed that `main()` was reached
immediately every time, including with the completely stock clock config
the "hang" was blamed on. The real cause was Zephyr's idle-thread `WFI`
combined with an ST-Link reconnect quirk, unrelated to firmware at all.
The lesson (already captured in the H723ZG FINDINGS.md itself, but worth
repeating): verify a "hang" with a debugger attached before building a fix
for it.

## Ran a "fix retest" that didn't actually retest the fix

Testing the ST community forum's specific `SDMMC_Init()` reinit workaround
(`FINDINGS.md`, "The actual ST-confirmed known issue" section), the first
attempt left `hsd1.Init.BusWide` at `1B` from an earlier, unrelated test —
so neither of the sequence's two `HAL_SD_ConfigWideBusOperation()` calls
was a genuine first switch to 4-bit; both were degenerate no-ops. The
result (still `DCRCFAIL`) wasn't wrong, but it also wasn't a real test of
the fix. Caught before relying on it, and redone faithfully (direct 4-bit
`BusWide` set before the first `HAL_SD_Init()` call, matching the original
forum post's exact starting condition) — same result held up, but only the
second attempt actually proved it.

## Trusted a card's volume label from memory instead of checking it

During the parent repo's bit-bang-driver side of BLOCKER-011
(`.buddy-project/blockers.md`), a card's FAT32 volume label was initially
misremembered/mislabeled rather than read back from the actual ground-truth
dump. Caught by cross-checking against the volume-label bytes baked into
the independently-verified-correct 1-bit ground truth. Since then, every
card-identity claim in this investigation has been backed by an actual CID/
CSD register dump or a direct host-side read, not memory.

## Declared "wiring confirmed correct" based on a test that couldn't have shown a wiring problem

After moving the F401 known-good harness to the H723ZG and running a clean
1-bit sweep, declared the harness's wiring fully verified. This was wrong:
1-bit mode only exercises CMD/CLK/D0 — it never asserts D1, D2, or D3, so a
swapped or open D1-D3 line couldn't have been caught by that test at all.
Caught on review before the claim went to ST or into a final conclusion;
resolved properly afterward with a direct Saleae capture and a per-lane
permutation check against known-good content (`captures/f401-harness/`),
which did independently confirm correct pin mapping. Lesson: identify what
a test can and can't rule out before citing it as having ruled something
out.

## Reached for "electrical crosstalk" before checking which side of the wire was corrupted

On finding a single-bit corruption on the `CMD17` command frame itself,
the first explanation reached for was external crosstalk/simultaneous-
switching noise from the data lines — and a sentence to that effect
(claiming a crosstalk problem "should improve at lower frequencies /
slower edges") went into `FINDINGS.md` before being checked. Both parts
were off: edge rate is set by GPIO slew rate, not by `SDMMC_CK` frequency,
so the "slower edges at low speed" reasoning doesn't hold; and more
importantly, the corrupted `CMD17` frame is entirely **host-driven** (the
MCU drives CMD outbound for a command), which makes external noise a much
less likely explanation than an internal peripheral sequencing issue.
Caught via direct pushback ("it's the same harness as the F401, why would
this be electrical, at such a low clock speed?") and corrected in
`FINDINGS.md`. Lesson: check which side is actively driving a corrupted
signal before reaching for an external-noise explanation.

## Re-derived a physical channel mapping the user had already given us

While setting up a Saleae MCP-driven capture, started writing a script to
determine the Saleae's raw-channel-to-D0/D1/D2/D3 mapping by brute-force
permutation search, despite the user having already stated the mapping
directly ("2-5 is D0/D1/D2/D3 in order, it's the same as all of the other
captures we did"). The tool call was rejected. Minor, but a real instance
of not using information already provided.

## Moved D0 to the "properly matched" pin, made the result worse, learned nothing

Early in the SDMMC1-peripheral investigation, D0 was moved from PB13
(CubeMX's actual assignment, reached via solder bridge SB8, not part of
the matched SDMMC trace group) to PC8 (the pin the board's Zio connector
groups with the rest of the matched bus), on the theory that testing on
a "properly matched" pin might change the result. It made things worse:
at full clock speed `BSP_SD_Init()` itself started failing, and at
reduced speed the read still failed and dropped *more* data than the
PB13 case. This was confounded by PC8 only being reachable via a flying
jumper on this board/adapter, versus header-to-header for every other
pin — so the result said more about connection quality than about which
pin was "correct." Reverted to PB13. Lesson: changing two variables at
once (pin *and* connection method) makes a result uninterpretable even
when it's a real, reproducible measurement.

## Claimed the physical layer was exonerated, based on the wrong pin

Every early claim that the physical layer had been ruled out (via 1-bit
bit-bang reads being byte-exact) was based on tests with D0 on PG10 —
not PB13, which is what the actual SDMMC1-peripheral test path used.
This went unnoticed until caught before finalizing the write-up for ST
support submission. Fixed by explicitly re-running the 1-bit control
test with D0 physically on PB13 before treating "the wiring is fine" as
an established fact — same general shape of mistake as the later
"wiring confirmed correct" one above (citing a test for more than it
actually covered), just earlier and about a different pin.

## Declared community research "exhausted" without checking a specific difference already on file

Asserted at one point that community-research leads were exhausted,
without checking a specific, real HAL-version code difference (in
`bkht`'s repo) that was already documented in this project's own
findings. Caught when asked directly "what about bkht's project?" — the
config difference was checked point-by-point and confirmed already
covered by other tests, but the lesson is to check and cite specifics
rather than assert a category is "exhausted."
