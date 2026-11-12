#!/usr/bin/env python3
"""Turn the output of a check_<effect>.py run into the markdown table of its acceptance section.

    python3 tools/fxbench/check_gate.py > run.txt
    python3 tools/fxbench/acceptance_table.py run.txt > table.md

fxcheck.Report prints every row twice: as it runs ("  PASS  check scope detail", check padded to 22
and scope to 34) and in the closing table (check padded to 24, scope to 36, then the status). The
two paddings differ, which is what locates the end of a check name longer than its column (the
shortest name that fits both prints is taken).
Works on the gate, comp, drive, chorus, phaser and tremolo outputs; check_eq.py prints extra lines in
the row format and is refused.
"""
import re
import sys


def rows(text):
    live = re.findall(r"^  (PASS|FAIL|INFO|TODO)  (.*)$", text, re.M)
    table = text.split(" TODO\n", 1)[1].splitlines()[1:len(live) + 1]
    if len(live) != len(table):
        sys.exit(f"{len(live)} rows printed while running, {len(table)} in the closing table")
    for (status, a), b in zip(live, table):
        head, detail = re.split(rf"{status} {{4}}", b, maxsplit=1)
        if not a.endswith(detail):
            sys.exit(f"the two prints of a row disagree: {a[:60]}")
        a = a[: len(a) - len(detail)]
        found = [(a[:n], a[max(n, 22) + 1:].rstrip()) for n in range(1, len(a))
                 if a[n - 1] != " " and not a[n:max(n, 22) + 1].strip()]
        found = [(c, s) for c, s in found if (c.ljust(24) + s).rstrip() == head.rstrip()]
        if not found:
            sys.exit(f"cannot split check from scope: {a[:80]}")
        yield status, found[0][0], found[0][1], detail.strip()


def cell(s):
    return s.replace("|", "\\|")


def main():
    text = open(sys.argv[1]).read()
    print("| Check | Scope | | Bar and figure, as the script prints them |")
    print("|---|---|---|---|")
    for status, check, scope, detail in rows(text):
        print(f"| {cell(check)} | {cell(scope)} | {status} | {cell(detail)} |")


if __name__ == "__main__":
    main()
