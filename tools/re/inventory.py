#!/usr/bin/env python3
"""Inventory installed Audio Units for reverse-engineering triage.

One TSV row per AudioComponent: identity, binary size, whether the arm64 code is
readable (byte entropy of __TEXT,__text), how many C++ symbols it ships, and which
copy-protection or framework strings it carries. Read-only: never loads a plugin.

    python3 tools/re/inventory.py > inventory.tsv
"""
import math
import plistlib
import struct
import subprocess
import sys
from pathlib import Path

ROOTS = [Path("/Library/Audio/Plug-Ins/Components"),
         Path.home() / "Library/Audio/Plug-Ins/Components"]

FAT_MAGIC, FAT_MAGIC_64 = 0xCAFEBABE, 0xCAFEBABF
MH_MAGIC_64 = 0xFEEDFACF
CPU_ARM64 = 0x0100000C
LC_SEGMENT_64 = 0x19

MARKERS = {
    "pace": [b"com.paceap", b"PACE Anti-Piracy", b"iLok"],
    "juce": [b"juce::", b"JUCE"],
    "iplug": [b"IPlug", b"iplug"],
    "qt": [b"QtCore", b"QObject"],
    "faust": [b"faust", b"FAUST"],
    "vdsp": [b"vDSP_"],
}


def arm64_slice(data):
    """(offset, size) of the arm64 Mach-O inside `data`, or None."""
    magic = struct.unpack(">I", data[:4])[0]
    if magic in (FAT_MAGIC, FAT_MAGIC_64):
        count = struct.unpack(">I", data[4:8])[0]
        pos = 8
        for _ in range(count):
            if magic == FAT_MAGIC:
                cpu, _sub, off, size, _align = struct.unpack(">iiIII", data[pos:pos + 20])
                pos += 20
            else:
                cpu, _sub, off, size, _align, _res = struct.unpack(">iiQQII", data[pos:pos + 32])
                pos += 32
            if cpu == CPU_ARM64:
                return off, size
        return None
    if struct.unpack("<I", data[:4])[0] == MH_MAGIC_64:
        cpu = struct.unpack("<i", data[4:8])[0]
        return (0, len(data)) if cpu == CPU_ARM64 else None
    return None


def text_section(data, base):
    """(file offset, size) of __TEXT,__text in the Mach-O starting at `base`."""
    ncmds = struct.unpack("<I", data[base + 16:base + 20])[0]
    pos = base + 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack("<II", data[pos:pos + 8])
        if cmd == LC_SEGMENT_64:
            segname = data[pos + 8:pos + 24].rstrip(b"\0")
            nsects = struct.unpack("<I", data[pos + 64:pos + 68])[0]
            sp = pos + 72
            for _ in range(nsects):
                sectname = data[sp:sp + 16].rstrip(b"\0")
                size, offset = struct.unpack("<QI", data[sp + 40:sp + 52])
                if segname == b"__TEXT" and sectname == b"__text":
                    return base + offset, size
                sp += 80
        pos += cmdsize
    return None


def entropy(chunk):
    if not chunk:
        return 0.0
    counts = [0] * 256
    for b in chunk:
        counts[b] += 1
    n = len(chunk)
    return -sum(c / n * math.log2(c / n) for c in counts if c)


def fourcc(v):
    if isinstance(v, int):
        return struct.pack(">I", v).decode("latin1")
    return str(v)


def symbol_count(binary):
    try:
        out = subprocess.run(["nm", "-arch", "arm64", "-U", "-j", str(binary)],
                             capture_output=True, text=True, timeout=120).stdout
    except Exception:
        return -1
    return sum(1 for line in out.splitlines() if line.startswith("__Z"))


def main():
    cols = ["bundle", "name", "type", "subtype", "manu", "binary", "size_mb", "arm64",
            "text_mb", "text_entropy", "cxx_symbols", "markers", "verdict"]
    print("\t".join(cols))
    for root in ROOTS:
        for bundle in sorted(root.glob("*.component")):
            plist_path = bundle / "Contents/Info.plist"
            try:
                info = plistlib.loads(plist_path.read_bytes())
            except Exception:
                continue
            binary = bundle / "Contents/MacOS" / info.get("CFBundleExecutable", bundle.stem)
            if not binary.exists():
                continue
            data = binary.read_bytes()
            sl = arm64_slice(data)
            text_mb, ent = 0.0, 0.0
            if sl:
                ts = text_section(data, sl[0])
                if ts:
                    off, size = ts
                    text_mb = size / 1e6
                    step = max(1, size // (4 << 20))
                    ent = entropy(data[off:off + size:step])
            found = [k for k, pats in MARKERS.items() if any(p in data for p in pats)]
            syms = symbol_count(binary) if sl else 0
            if not sl:
                verdict = "no-arm64"
            elif ent > 7.5:
                verdict = "encrypted"
            elif text_mb < 0.3:
                verdict = "stub"
            elif syms > 2000:
                verdict = "readable+symbols"
            else:
                verdict = "readable"
            comps = info.get("AudioComponents") or [{}]
            for comp in comps:
                row = [bundle.name, comp.get("name", ""), fourcc(comp.get("type", "")),
                       fourcc(comp.get("subtype", "")), fourcc(comp.get("manufacturer", "")),
                       str(binary), f"{len(data) / 1e6:.1f}", "yes" if sl else "no",
                       f"{text_mb:.1f}", f"{ent:.2f}", str(syms), ",".join(found), verdict]
                print("\t".join(row))
            sys.stdout.flush()


if __name__ == "__main__":
    main()
