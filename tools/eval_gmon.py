"""Resolve GNU gmon call PCs against full ELF ranges, including GCC LTO clones.

Linux x86-64 little-endian only. Histogram buckets are approximate self samples,
not wall time; call counts are counts in the instrumented binary, not release.
"""
import bisect
from collections import Counter
from pathlib import Path
import struct
import subprocess


def decode(data, symbols):
    if data[:4] != b"gmon" or len(data) < 20 or struct.unpack_from("<I", data, 4)[0] != 1:
        raise ValueError("expected GNU gmon version 1")
    symbols = sorted(symbols)
    starts = [s[0] for s in symbols]

    def resolve(pc):
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0 and pc < symbols[i][0] + symbols[i][1]:
            return symbols[i][2]
        return "<unmapped>"

    pos, rate = 20, None
    calls, ticks = Counter(), Counter()
    while pos < len(data):
        tag = data[pos]
        pos += 1
        if tag == 0:
            low, high, size, current_rate = struct.unpack_from("<QQII", data, pos)
            if not size or high <= low or not current_rate or (rate and rate != current_rate):
                raise ValueError("invalid histogram geometry/rate")
            rate = current_rate
            pos += 40  # PCs, size, rate, 15-byte dimension, one-byte abbreviation.
            for i, count in enumerate(struct.unpack_from(f"<{size}H", data, pos)):
                if count:
                    # Bucket midpoint; retain unassigned buckets rather than crediting a neighbor.
                    ticks[resolve(low + (2 * i + 1) * (high - low) // (2 * size))] += count
            pos += 2 * size
        elif tag == 1:
            _, callee, count = struct.unpack_from("<QQI", data, pos)
            calls[resolve(callee)] += count
            pos += 20
        else:
            raise ValueError(f"unsupported gmon record {tag}")
    return {"sample_rate_hz": rate, "self_ticks": dict(ticks), "calls": dict(calls),
            "note": "instrumented-binary counts; histogram buckets resolved by midpoint; libc excluded"}


def inspect(binary, gmon):
    with Path(binary).open("rb") as stream:
        header = stream.read(20)
    if header[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", header, 18)[0] != 62:
        raise ValueError("gmon decoder supports only little-endian ELF x86-64")
    nm = subprocess.check_output(["nm", "-n", "-S", "--defined-only", str(binary)], text=True)
    symbols = []
    for line in nm.splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[2] in ("t", "T"):
            symbols.append((int(fields[0], 16), int(fields[1], 16), fields[3]))
    if not symbols:
        raise ValueError("profile binary has no sized text symbols")
    result = decode(Path(gmon).read_bytes(), symbols)
    result["symbol_source"] = "nm -n -S --defined-only; full half-open function intervals"
    return result
