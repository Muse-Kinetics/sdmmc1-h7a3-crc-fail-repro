#!/usr/bin/env python3
"""Decode a 4-bit SD bus logic-analyzer capture (CMD,CLK,D0-D3 on-change CSV,
Saleae Logic2 export format: "Time [s],CMD,CLK,D0,D1,D2,D3") to find
READ_SINGLE/MULTIPLE_BLOCK commands, and check the resulting data block
against known-good ground truth.

No third-party dependencies (stdlib only) - matches what's used to produce
read_lba8192_clockdiv40.csv's analysis in FINDINGS.md.

Usage:
    python3 decode_capture.py read_lba8192_clockdiv40.csv

This capture was taken with SDMMC1.ClockDiv temporarily raised to 40
(~450kHz) instead of the real-world 0 (~36MHz) specifically so an 8MHz-class
logic analyzer (24MHz hardware ceiling on the unit used here) can resolve it
cleanly - ClockDiv is a config already shown elsewhere in FINDINGS.md to fail
identically, so this doesn't change what's being captured.
"""
import sys

# Known-good ground truth for LBA 8192 on the primary test card (a FAT32
# volume boot record), obtained by reading the card directly on a Mac/Linux
# host, bypassing the MCU entirely - see FINDINGS.md, "Card and
# physical-layer verification". Update this if decoding a capture of a
# different LBA/card.
GROUND_TRUTH_LBA8192_HEX = """
eb58 9042 5344 2020 342e 3400 0220 2000
0200 0000 00f8 0000 2000 ff00 0020 0000
00c0 a303 353a 0000 0000 0000 0200 0000
0100 0600 0000 0000 0000 0000 0000 0000
8000 2919 1995 8856 4f4c 554d 4520 2020
2020 4641 5433 3220 2020 fa31 c08e d0bc
007c fb8e d8e8 0000 5e83 c619 bb07 00fc
ac84 c074 06b4 0ecd 10eb f530 e4cd 16cd
190d 0a4e 6f6e 2d73 7973 7465 6d20 6469
736b 0d0a 5072 6573 7320 616e 7920 6b65
7920 746f 2072 6562 6f6f 740d 0a00 0000
"""
# followed by zero-fill through byte 509, then 55 AA at 510-511.


def crc7_bit(crc, bit):
    bit ^= (crc >> 6) & 1
    crc = (crc << 1) & 0x7F
    if bit:
        crc ^= 0x09
    return crc


def crc7(bits):
    crc = 0
    for b in bits:
        crc = crc7_bit(crc, b)
    return crc


def crc16_bit(crc, bit):
    bit ^= (crc >> 15) & 1
    crc = (crc << 1) & 0xFFFF
    if bit:
        crc ^= 0x1021
    return crc


def crc16(bits):
    crc = 0
    for b in bits:
        crc = crc16_bit(crc, b)
    return crc


def build_ground_truth_bytes():
    gt = bytearray()
    for tok in GROUND_TRUTH_LBA8192_HEX.split():
        gt.append(int(tok[0:2], 16))
        gt.append(int(tok[2:4], 16))
    while len(gt) < 510:
        gt.append(0)
    gt += bytes([0x55, 0xAA])
    return bytes(gt[:512])


def load_clk_rising_edge_samples(path):
    """Returns a list of (cmd, d0, d1, d2, d3) sampled at each CLK
    rising edge, in chronological order."""
    samples = []
    prev_clk = None
    with open(path, "r") as f:
        f.readline()  # header
        for line in f:
            parts = line.rstrip("\n").split(",")
            clk = int(parts[2])
            if prev_clk == 0 and clk == 1:
                samples.append((int(parts[1]), int(parts[3]), int(parts[4]),
                                 int(parts[5]), int(parts[6])))
            prev_clk = clk
    return samples


NAME_MAP = {
    0: "GO_IDLE_STATE", 2: "ALL_SEND_CID", 3: "SEND_RELATIVE_ADDR",
    6: "SET_BUS_WIDTH(ACMD6)/SWITCH_FUNC(CMD6)", 7: "SELECT_CARD",
    8: "SEND_IF_COND", 9: "SEND_CSD", 13: "SEND_STATUS", 16: "SET_BLOCKLEN",
    17: "READ_SINGLE_BLOCK", 18: "READ_MULTIPLE_BLOCK",
    41: "SD_SEND_OP_COND(ACMD41)", 55: "APP_CMD",
}


def find_command_frames(cmd_bits):
    """Scan the CMD line's rising-edge-sampled bitstream for valid host
    command frames (start=0, transmit=1, 6-bit index, 32-bit arg, CRC7,
    end=1), validated by CRC7 to reject false positives."""
    found = []
    n = len(cmd_bits)
    i = 0
    while i < n - 48:
        if cmd_bits[i] == 0 and cmd_bits[i + 1] == 1:
            frame = cmd_bits[i:i + 48]
            idx_bits, arg_bits, crc_bits, end_bit = (
                frame[2:8], frame[8:40], frame[40:47], frame[47])
            if end_bit == 1 and crc7(frame[0:40]) == int("".join(map(str, crc_bits)), 2):
                cmd_index = int("".join(map(str, idx_bits)), 2)
                arg = int("".join(map(str, arg_bits)), 2)
                found.append((i, cmd_index, arg))
                i += 48
                continue
        i += 1
    return found


def find_data_block_start(d_bits, search_from, search_to):
    """First sample where all four D lines read 0 (the data block's start
    bit) after a command's response phase."""
    for j in range(search_from, search_to):
        if d_bits[j] == (0, 0, 0, 0):
            return j
    return None


def reconstruct_bytes(d_bits, data_start, lane_order=(3, 2, 1, 0)):
    """Assemble the 512-byte block from 4x 1024-bit lanes, canonical SD
    4-bit-mode nibble order (D3=bit3..D0=bit0, MSB nibble first per byte)."""
    out = bytearray()
    nibbles = []
    for cyc in range(1024):
        bits = d_bits[data_start + cyc]
        v = 0
        for w, lane in enumerate(lane_order):
            v |= (bits[lane] << (3 - w))
        nibbles.append(v)
    for i in range(0, 1024, 2):
        out.append((nibbles[i] << 4) | nibbles[i + 1])
    return bytes(out)


def find_best_alignment(d_bits, start_bit_idx, expected_nibbles, lane_order=(3, 2, 1, 0),
                         search_margin=30):
    """Slides the reconstructed nibble stream against `expected_nibbles`
    over a window of `search_margin` cycles before the detected start bit,
    to absorb off-by-a-few-cycles error in start-bit detection itself.
    Returns (best_offset, match_count, aligned_nibbles)."""
    begin = max(0, start_bit_idx - search_margin)
    end = min(len(d_bits), start_bit_idx + 1 + 1024 + 16 + 15)
    nib = []
    for c in range(begin, end):
        bits = d_bits[c]
        v = 0
        for w, lane in enumerate(lane_order):
            v |= (bits[lane] << (3 - w))
        nib.append(v)

    best = None
    for off in range(0, len(nib) - 1024):
        matches = sum(1 for k in range(1024) if nib[off + k] == expected_nibbles[k])
        if best is None or matches > best[0]:
            best = (matches, off)
    return best[0], nib[best[1]:best[1] + 1024]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "read_lba8192_clockdiv40.csv"
    print(f"Loading {path} ...")
    samples = load_clk_rising_edge_samples(path)
    print(f"CLK rising-edge samples: {len(samples)}")

    cmd_bits = [s[0] for s in samples]
    d_bits = [(s[1], s[2], s[3], s[4]) for s in samples]

    commands = find_command_frames(cmd_bits)
    print(f"\nValid host command frames: {len(commands)}")
    for (idx, cmd_index, arg) in commands:
        print(f"  sample#{idx:6d}  CMD{cmd_index:<3d} {NAME_MAP.get(cmd_index, ''):<40s} arg=0x{arg:08X}")

    reads = [c for c in commands if c[1] in (17, 18)]
    if not reads:
        print("\nNo READ_SINGLE_BLOCK/READ_MULTIPLE_BLOCK found in this capture.")
        return

    cmd_start_idx, cmd_index, arg = reads[-1]  # last one - closest to the data phase
    print(f"\n=== Analyzing CMD{cmd_index} arg=0x{arg:08X} ===")
    cmd_end_idx = cmd_start_idx + 48
    start_bit_idx = find_data_block_start(d_bits, cmd_end_idx, min(len(d_bits), cmd_end_idx + 20000))
    if start_bit_idx is None:
        print("No data-block start bit found - read errored before the data phase, or capture too short.")
        return
    print(f"Data start bit at sample#{start_bit_idx} ({start_bit_idx - cmd_end_idx} clocks after CMD end)")

    # Per-lane CRC16 self-check, independent of the MCU/HAL.
    data_start = start_bit_idx + 1
    if data_start + 1024 + 16 + 1 <= len(d_bits):
        print("\nPer-lane (D0..D3) CRC16 self-check on the captured wire bits:")
        all_match = True
        for lane in range(4):
            databits = [d_bits[data_start + k][lane] for k in range(1024)]
            crcbits = [d_bits[data_start + 1024 + k][lane] for k in range(16)]
            calc = crc16(databits)
            recv = int("".join(map(str, crcbits)), 2)
            match = calc == recv
            all_match &= match
            print(f"  D{lane}: calc=0x{calc:04X} recv=0x{recv:04X}  {'MATCH' if match else 'MISMATCH'}")
        print("=> Wire data is self-consistent (peripheral's CRC-fail would implicate the silicon)."
              if all_match else
              "=> Wire data does NOT pass its own CRC16 - real bit-level corruption in transit.")

    # Byte-level reconstruction vs ground truth, if this read matches the
    # known test address (LBA 8192 in this repro).
    if arg == 0x2000:
        expected = []
        for b in build_ground_truth_bytes():
            expected.append((b >> 4) & 0xF)
            expected.append(b & 0xF)
        matches, aligned_nibbles = find_best_alignment(d_bits, start_bit_idx, expected)
        recon = bytes((aligned_nibbles[i] << 4) | aligned_nibbles[i + 1] for i in range(0, 1024, 2))
        gt = build_ground_truth_bytes()
        print(f"\nBest alignment against known-good LBA 8192 content: {matches}/1024 nibbles match")
        mism = [i for i in range(512) if recon[i] != gt[i]]
        if mism:
            print(f"Mismatched bytes: {len(mism)}/512, first={mism[0]}, last={mism[-1]}")
            # report the longest exact-match run from the end, which is
            # usually the most convincing evidence of a clean tail
            i = 511
            while i > 0 and recon[i] == gt[i]:
                i -= 1
            print(f"Clean matching run: bytes {i+1}-511 ({511-i} bytes), "
                  f"ending in {recon[510]:02x}{recon[511]:02x} "
                  f"(boot signature, expected 55aa)")
        else:
            print("Full 512-byte block matches ground truth exactly (still fails hardware CRC16 - "
                  "see FINDINGS.md for how that's possible: framing/CRC-window alignment, not content).")


if __name__ == "__main__":
    main()
