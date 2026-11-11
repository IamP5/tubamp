#!/usr/bin/env python3
"""Symbol-guided arm64 disassembly for binaries that ship C++ symbols.

Parallel-safe (plain nm + objdump, no Hopper). The symbol index and the thin
arm64 slice are cached under $RE_STATE.

    disasm.py <binary> --list  <regex>     matching symbols: address, size, name
    disasm.py <binary> --dump  <regex>     disassemble every match (cap with --max)
    disasm.py <binary> --addr  0x1234      disassemble the symbol containing an address
    disasm.py <binary> --floats 0x1000 64  read N little-endian float32 at a vm address
    disasm.py <binary> --doubles 0x1000 8  read N float64 at a vm address
    disasm.py <binary> --strings <regex>   C strings matching a regex

The regex is matched case-insensitively against the demangled name.
"""
import argparse
import bisect
import hashlib
import os
import re
import struct
import subprocess
import sys
from pathlib import Path


def state_dir():
    root = os.environ.get("RE_STATE")
    if not root:
        sys.exit("set RE_STATE to the shared state directory")
    return Path(root)


def cache_key(binary):
    stamp = f"{binary}|{int(binary.stat().st_mtime)}|arm64"
    return hashlib.sha1(stamp.encode()).hexdigest()[:16]


def thin_slice(binary):
    out = state_dir() / "thin" / f"{cache_key(binary)}-{binary.name}"
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        return out
    info = subprocess.run(["lipo", "-info", str(binary)], capture_output=True, text=True).stdout
    if "Non-fat" in info:
        out.symlink_to(binary)
        return out
    tmp = out.with_name(out.name + f".tmp.{os.getpid()}")
    subprocess.run(["lipo", "-thin", "arm64", str(binary), "-output", str(tmp)], check=True)
    tmp.replace(out)
    return out


def symbol_index(binary):
    """Sorted [(address, name)] of defined symbols in the arm64 slice."""
    path = state_dir() / "symidx" / f"{cache_key(binary)}.txt"
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        nm = subprocess.run(["nm", "-arch", "arm64", "-U", "-n", str(binary)],
                            capture_output=True, text=True).stdout
        filt = subprocess.run(["c++filt"], input=nm, capture_output=True, text=True).stdout
        tmp = path.with_name(path.name + f".tmp.{os.getpid()}")
        tmp.write_text(filt)
        tmp.replace(path)
    syms = []
    for line in path.read_text().splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and len(parts[1]) == 1:
            try:
                syms.append((int(parts[0], 16), parts[1], parts[2]))
            except ValueError:
                pass
    syms.sort()
    return syms


def sections(thin):
    """[(segname, sectname, vmaddr, size, fileoff)] of a thin 64-bit Mach-O."""
    data = thin.read_bytes()[:1 << 20]
    ncmds = struct.unpack("<I", data[16:20])[0]
    pos, out = 32, []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack("<II", data[pos:pos + 8])
        if cmd == 0x19:
            nsects = struct.unpack("<I", data[pos + 64:pos + 68])[0]
            sp = pos + 72
            for _ in range(nsects):
                sect = data[sp:sp + 16].rstrip(b"\0").decode()
                seg = data[sp + 16:sp + 32].rstrip(b"\0").decode()
                addr, size, off = struct.unpack("<QQI", data[sp + 32:sp + 52])
                out.append((seg, sect, addr, size, off))
                sp += 80
        pos += cmdsize
    return out


def read_vm(thin, addr, length):
    for _seg, _sect, vmaddr, size, off in sections(thin):
        if vmaddr <= addr < vmaddr + size and off:
            with open(thin, "rb") as fh:
                fh.seek(off + addr - vmaddr)
                return fh.read(length)
    sys.exit(f"address {addr:#x} is not in a file-backed section")


def text_end(thin):
    for seg, sect, vmaddr, size, _off in sections(thin):
        if seg == "__TEXT" and sect == "__text":
            return vmaddr + size
    return None


def code_symbols(syms):
    return [(a, n) for a, kind, n in syms if kind in "tT"]


def extent(code, i, end):
    return (code[i + 1][0] if i + 1 < len(code) else end) - code[i][0]


def dump(thin, start, stop):
    out = subprocess.run(["objdump", "-d", "--no-show-raw-insn",
                          f"--start-address={start:#x}", f"--stop-address={stop:#x}", str(thin)],
                         capture_output=True, text=True).stdout
    body = out.split("\n", 6)[-1] if out.count("\n") > 6 else out
    return body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--list")
    mode.add_argument("--dump")
    mode.add_argument("--addr")
    mode.add_argument("--floats", nargs=2, metavar=("ADDR", "N"))
    mode.add_argument("--doubles", nargs=2, metavar=("ADDR", "N"))
    mode.add_argument("--strings")
    ap.add_argument("--max", type=int, default=12, help="most symbols to dump")
    ap.add_argument("--max-bytes", type=int, default=24000, help="skip symbols larger than this")
    args = ap.parse_args()

    binary = Path(args.binary)
    thin = thin_slice(binary)

    if args.floats or args.doubles:
        addr_s, n_s = args.floats or args.doubles
        width, fmt = (4, "f") if args.floats else (8, "d")
        raw = read_vm(thin, int(addr_s, 16), int(n_s) * width)
        vals = struct.unpack(f"<{len(raw) // width}{fmt}", raw)
        for i, v in enumerate(vals):
            print(f"{int(addr_s, 16) + i * width:#x}\t{v!r}")
        return

    if args.strings:
        rx = re.compile(args.strings, re.I)
        out = subprocess.run(["strings", "-a", "-arch", "arm64", str(binary)],
                             capture_output=True, text=True, errors="replace").stdout
        for line in out.splitlines():
            if rx.search(line):
                print(line)
        return

    syms = symbol_index(binary)
    code = code_symbols(syms)
    end = text_end(thin) or (code[-1][0] + 4)

    if args.addr:
        addr = int(args.addr, 16)
        i = bisect.bisect_right([a for a, _ in code], addr) - 1
        if i < 0:
            sys.exit("address is before the first symbol")
        print(f"; {code[i][1]}  @ {code[i][0]:#x}  ({extent(code, i, end)} bytes)")
        print(dump(thin, code[i][0], code[i][0] + extent(code, i, end)))
        return

    rx = re.compile(args.list or args.dump, re.I)
    hits = [i for i, (_a, n) in enumerate(code) if rx.search(n)]
    if args.list:
        for i in hits:
            print(f"{code[i][0]:#x}\t{extent(code, i, end)}\t{code[i][1]}")
        print(f"# {len(hits)} symbols", file=sys.stderr)
        return

    shown = 0
    for i in hits:
        size = extent(code, i, end)
        if size > args.max_bytes:
            print(f"; SKIPPED {code[i][1]} @ {code[i][0]:#x}: {size} bytes > --max-bytes")
            continue
        if shown >= args.max:
            note = f"stopped at --max {args.max}; {len(hits) - shown} matches not shown"
            print(f"; {note}")
            print(f"disasm.py: {note}", file=sys.stderr)
            break
        print(f"; ===== {code[i][1]}  @ {code[i][0]:#x}  ({size} bytes)")
        print(dump(thin, code[i][0], code[i][0] + size))
        shown += 1


if __name__ == "__main__":
    main()
