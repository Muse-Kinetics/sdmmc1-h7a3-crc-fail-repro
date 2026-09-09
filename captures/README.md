# Logic-analyzer captures

`read_lba8192_clockdiv40.csv` — a 4-bit SD bus capture (CMD, CLK, D0-D3)
of the exact failing transaction, taken with `SDMMC1.ClockDiv` temporarily
raised to 40 (~450kHz instead of the real-world 0/~36MHz) so an 8MHz-class
logic analyzer can resolve it cleanly. `ClockDiv` is a config already shown
in [../FINDINGS.md](../FINDINGS.md) to fail identically to full speed, so
this doesn't change what's being captured — only makes it visible to the
analyzer used here (24MHz hardware ceiling, well under Nyquist for a native
36MHz capture).

Format: Saleae Logic2's CSV export, `Time [s],CMD,CLK,D0,D1,D2,D3`, one row
per logic-level change on any channel.

`decode_capture.py` (stdlib only, no dependencies) decodes it:

```bash
python3 decode_capture.py read_lba8192_clockdiv40.csv
```

It reconstructs the SD command stream (validated via CRC7), locates the
`READ_SINGLE_BLOCK` command and its data phase, runs a per-lane CRC16
self-check on the captured bits (independent of the MCU), and — for the
LBA 8192 read this capture contains — reconstructs the actual 512-byte
block and diffs it against the known-good ground truth from
[../FINDINGS.md](../FINDINGS.md#card-and-physical-layer-verification).

See FINDINGS.md's "Saleae capture: corruption is on the wire" section for
what this specific capture showed and why it matters.
