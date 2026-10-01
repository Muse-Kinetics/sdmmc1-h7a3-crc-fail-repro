# F401-harness-on-H723ZG captures

The known-good F401/SDIO harness (`Bringup/F401-known-good/sdio-test`),
moved unmodified to a NUCLEO-H723ZG, capturing CMD/CLK/D0-D3 during a
single fixed-clock (`ClockDiv=60`, 400kHz) 4-bit `CMD17` read of LBA
8192. Decode with the copy of the decoder in this directory (it has the
correct ground-truth bytes for *this* card, not the original H7A3
capture's card):

```bash
python3 decode_capture.py <capture>.csv
```

Two captures, from independent reset cycles:

- **`h723zg_lba8192_clockdiv60.csv`** — first capture (manual Logic2 GUI).
- **`h723zg_lba8192_clockdiv60_repeat2.csv`** — repeat, captured
  remotely via the Saleae Automation API/MCP server (raw channels
  reordered to `Time,CMD,CLK,D0,D1,D2,D3` for the decoder — CMD=raw ch0,
  CLK=raw ch1, D0-D3=raw ch2-5, this bench's standard mapping).

Both show the same data-block corruption byte-for-byte identical
(897/1024 nibbles, corrupted range bytes 0-111, rest matching ground
truth including the boot signature) and confirm D0-D3 pin mapping is
correct. The CMD line shows corruption in both, but a different pattern
each time — a clean single-bit flip in the first capture, a messier,
non-CRC-validating pattern in the second. See `FINDINGS.md` ("Round 4")
for the full analysis and what that asymmetry might mean.
