# st-sd-ref — SDMMC1 4-bit SD/FAT access fails (NUCLEO-H7A3ZI-Q)

## Major update (2026-09-29) — read this before the rest of this README

Everything below this point describes the state of the investigation as
filed with ST (ticket 00268848) and is kept for reproducibility, but **the
conclusion has changed substantially since, and the "please advise, we're
out of ideas" framing below is no longer accurate.** Two things changed it:

1. **The most-cited evidence for a peripheral/silicon defect (a Saleae
   capture allegedly showing single-bit corruption on the command line) was
   retracted** — it was a logic-analyzer decode artifact (a sampling-timing
   ambiguity at the capture's sample rate), not real corruption. Do not cite
   it.
2. **A designed experiment found the 4-bit data-line failure is a precisely
   characterized simultaneous-switching effect, and moving the same card to
   a real PCB with a built-in SD slot (not a bare-wire breakout adapter,
   like this repro uses) reads 100% clean at every speed and content
   pattern that failed 100% of the time here** — including the worst-case
   stress pattern.

**Current conclusion: this reads as a board-layout/decoupling-quality issue
specific to bare-wire breakout adapters (exactly this repro's own hardware
setup), not a defect in the SDMMC v2 peripheral, the H7 chip family, or the
driver code below.** Full detail: `FINDINGS.md`'s "Round 5" and "Round 6"
sections. If re-engaging ST, the honest ask is now "we characterized a
board-layout-dependent signal-integrity effect on our own bare-wire test
harness, and want to confirm whether it's expected behavior for this class
of physical connection" — not "please find the peripheral/driver bug,"
which is what everything below still asks.

## Problem

Basic SD card / FAT access on a NUCLEO-H7A3ZI-Q — CubeMX-generated
`HAL_SD` + FatFs, SDMMC1 in 4-bit mode fails every time. `f_mount()` 
returns `FR_DISK_ERR` because the very first sector read fails with 
`SDMMC_ERROR_DATA_CRC_FAIL`.

The card and the wiring have both been independently verified good, and
a logic-analyzer capture of the actual failing transfer confirms the
corruption is real and on the wire — but only in **4-bit mode**; the
same net reads byte-exact in 1-bit mode on an independent driver (details
in [FINDINGS.md](FINDINGS.md)). We're out of host-side/software things to
try. Please advise.

## Reproduce

1. Wire a microSD breakout per [Pinout](#pinout) below.
2. Build and flash (below).
3. Watch the ST-LINK VCP (115200 8N1, no debugger needed):
   ```
   [SD-REPRO] f_mount("0:/")...
   [SD-REPRO] f_mount() = 1 (FR_DISK_ERR)
   ```

That's the whole repro. `main.c` is stock CubeMX output plus a single
call to `SD_Repro_MountAndListRoot()` (mount the volume, list the root
directory) — no custom driver code runs before the failure.

### Build

Any current arm-none-eabi-gcc + CMake + Ninja toolchain (exercised with
GCC 14.3.1, CubeMX-bundled). From this directory:

```bash
cmake --preset Debug
cmake --build build/Debug
```

Produces `build/Debug/st-sd-ref.elf`. Plain CubeMX/CMake project — also
imports cleanly into STM32CubeIDE as an existing project.

### Flash

```bash
STM32_Programmer_CLI -c port=SWD -w build/Debug/st-sd-ref.elf -rst
```

Any ST-LINK-compatible tool works. Output is on the ST-LINK VCP
(USART3, 115200 8N1).

## Pinout

| Signal | STM32 pin | Connector | Pin # | SD card pin |
|---|---|---|---|---|
| CMD | PD2 | CN8 (Zio) | 12 | CMD |
| CLK | PC12 | CN8 (Zio) | 10 | CLK |
| D3 | PC11 | CN8 (Zio) | 8 | DAT3 |
| D2 | PC10 | CN8 (Zio) | 6 | DAT2 |
| D1 | PC9 | CN8 (Zio) | 4 | DAT1 |
| **D0** | **PB13** | **CN7 (Zio)** | **5** | **DAT0** |
| 3V3 | — | CN8 (Zio) | 7 | VDD |
| GND | — | CN8 (Zio) | 11 or 13 | VSS |

Source: UM2408 Rev 6, Table 18 ("NUCLEO-H7A3ZI-Q pin assignments") and
the `MB1363` board schematic, sheet 5.

**Why D0 is on PB13, not PC8:** PC8 is the pin physically grouped with
the rest of the matched SDMMC bus on the Zio connector, but it's
unavailable in this project (see [FINDINGS.md](FINDINGS.md) if that
matters to your setup) — PB13 is the only other valid `SDMMC1_D0`
alternate-function pin on this chip, and CubeMX picks it correctly.
We've independently verified the PB13 trace itself is electrically
clean (details in FINDINGS.md), so this isn't the cause of the failure
above.

## What we've already tried

Every host-side/software/config lever we could find — bus width, clock
speed (80x range), clock edge, D0 pin, `HardwareFlowControl`, polling
vs. IDMA, read address, a full sweep of the SDMMC1 RX delay-block's
sampling phase (1536 points), and three separate attempts at ST's
suggested init-sequencing fix (ticket 00268848), including the actual
ST-confirmed known-issue fix found on the ST community forum — either
made no difference or produced a different, separately-understood
failure. None fixed the read. See FINDINGS.md for why we believe our
failure is a different bug from the one ST's known-issue writeup
describes, and for an independent report of the same failure shape from
another ST community member on different H7 hardware.

Two pieces of direct evidence stand out:

- Reading the raw `STA`/`DCOUNT` registers at the moment of failure shows
  the peripheral's own accounting believes it received a complete,
  correctly-sized 512-byte block with zero FIFO overrun — and its own
  real-time CRC16 engine still rejects it.
- A logic-analyzer capture of the actual bus (in `captures/`) shows why:
  reconstructing the real 512-byte block from the captured wire and
  diffing it against known-good content shows the **back three-quarters
  of the block (387 bytes, including the closing boot signature) match
  exactly**, while the **front quarter shows real, physical bit-level
  corruption** — the same kind of dropped-nibble pattern already seen in
  an unrelated bit-bang-driver bug elsewhere in the parent repo.

Full detail, register dumps, and the reasoning behind each ruled-out
cause: **[FINDINGS.md](FINDINGS.md)**.

**Update since filing:** an independent RTOS (Zephyr) reproduces the
identical failure on a second STM32H7 chip, and a bare-wire test harness
proven clean in 4-bit mode on the older "SDIO" peripheral (STM32F401)
fails 100% of the time at every speed tested when moved unmodified to a
newer "SDMMC v2" chip (STM32H723). See FINDINGS.md's "Round 3" and
"Round 4" for detail — this evidence hasn't been re-reported to ST yet.

## Diagnostics included

`sd_diag.h`/`sd_diag.c` has the register-level tooling we used to
narrow this down (card info dump, raw STA/DCOUNT read, IDMA test, RX
delay-block sweep). Not run by default — uncomment the calls in
`main.c`'s `USER CODE WHILE` block to use them. See FINDINGS.md for what
each one showed.

`captures/` has a logic-analyzer capture of the actual failing transfer
plus the (dependency-free) Python script used to decode it — see
[captures/README.md](captures/README.md).


## License

Our own additions (`sd_diag.c`/`.h`, the CMake wiring, this README) are
MIT-licensed — see [LICENSE](LICENSE). The CubeMX-generated
HAL/CMSIS/BSP/FatFs scaffold under `Drivers/`, `Middlewares/`, and parts
of `Core/`/`FATFS/` keeps its original STMicroelectronics/ChaN
licensing — see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
