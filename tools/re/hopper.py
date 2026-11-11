#!/usr/bin/env python3
"""Hopper pseudo-code for many callers through one daemon and one shared cache.

Hopper is a single GUI app: it holds one analysed document at a time and a second
concurrent session hangs. So exactly one daemon owns it. Callers queue requests as
files; the daemon keeps a binary open while requests for it keep coming (re-analysing
a big binary costs minutes to hours) and every answer lands in a cache keyed by
(binary, address), so a repeated question never reaches Hopper.

    hopper.py <binary> 0x2a9610 [0x...]...        pseudo-code for those procedures
    hopper.py <binary> --match '<regex>'          every symbol whose demangled name matches
    hopper.py <binary> --match '<regex>' --no-wait   queue only (pre-warming)
    hopper.py --status                            daemon state and queue
    hopper.py --serve                             run the daemon (started on demand)

Batch your targets: one call with twenty addresses, not twenty calls. For binaries
with symbols, tools/re/disasm.py answers instantly and in parallel; reach for Hopper
when the control flow is easier to read as pseudo-code.

Needs RE_STATE (shared state directory). The daemon must be started from a shell
whose ancestor process is allowed to send Apple Events to Hopper.
"""
import argparse
import glob
import hashlib
import json
import os
import subprocess
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import disasm  # noqa: E402

STARTUP_TIMEOUT_MS = "10800000"
IDLE_EXIT_S = 6 * 3600


def root():
    path = disasm.state_dir() / "hopperd"
    (path / "req").mkdir(parents=True, exist_ok=True)
    return path


def cache_dir(key):
    path = disasm.state_dir() / "hopper-cache" / key
    path.mkdir(parents=True, exist_ok=True)
    return path


def target_file(key, target):
    name = target.lower() if target.lower().startswith("0x") else hashlib.sha1(target.encode()).hexdigest()[:16]
    return cache_dir(key) / f"{name}.txt"


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except (OSError, ValueError):
        return False


def daemon_pid():
    try:
        pid = int((root() / "pid").read_text().strip())
    except (OSError, ValueError):
        return None
    return pid if alive(pid) else None


def ensure_daemon():
    if daemon_pid():
        return
    lock = root() / "start.lock"
    try:
        lock.mkdir()
    except FileExistsError:
        if time.time() - lock.stat().st_mtime > 60:
            lock.rmdir()
            return ensure_daemon()
        for _ in range(100):
            if daemon_pid():
                return
            time.sleep(0.2)
        sys.exit("hopper.py: another caller is starting the daemon but it never came up")
    try:
        if not daemon_pid():
            log = open(root() / "daemon.log", "a")
            subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--serve"],
                             stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
            for _ in range(100):
                if daemon_pid():
                    break
                time.sleep(0.2)
            else:
                sys.exit("hopper.py: daemon did not start; see hopperd/daemon.log")
    finally:
        lock.rmdir()


# ---------------------------------------------------------------- daemon

class Rea:
    def __init__(self):
        rea = sorted(glob.glob(os.path.expanduser("~/.npm/_npx/*/node_modules/.bin/rea")), key=os.path.getmtime)[-1]
        env = dict(os.environ, REA_HOPPER_STARTUP_TIMEOUT_MS=STARTUP_TIMEOUT_MS)
        self.proc = subprocess.Popen([rea, "--mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=open(root() / "rea.log", "a"), text=True, bufsize=1,
                                     env=env, cwd=str(disasm.state_dir()))
        self.next_id = 0
        self.rpc("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                "clientInfo": {"name": "tubamp-hopperd", "version": "1"}})
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})

    def send(self, message):
        self.proc.stdin.write(json.dumps(message) + "\n")
        self.proc.stdin.flush()

    def rpc(self, method, params):
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params})
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("rea MCP server closed its output")
            try:
                reply = json.loads(line)
            except ValueError:
                continue
            if reply.get("id") == self.next_id:
                return reply

    def tool(self, name, arguments):
        """(ok, text) of one tool call."""
        reply = self.rpc("tools/call", {"name": name, "arguments": arguments})
        result = reply.get("result")
        if not isinstance(result, dict):
            return False, json.dumps(reply.get("error", reply))[:2000]
        text = "".join(part.get("text", "") for part in result.get("content", []))
        return not result.get("isError"), text

    def close(self):
        try:
            self.tool("close_binary", {})
            self.proc.stdin.close()
            self.proc.wait(timeout=20)
        except Exception:
            self.proc.kill()


def log(message):
    print(f"{time.strftime('%H:%M:%S')} {message}", flush=True)


def pending_requests():
    out = []
    for path in sorted((root() / "req").glob("*.json")):
        try:
            out.append((path, json.loads(path.read_text())))
        except (OSError, ValueError):
            continue
    return out


def serve():
    here = root()
    (here / "pid").write_text(str(os.getpid()))
    log(f"daemon up, pid {os.getpid()}")
    rea, current, load_s, last_served, idle_since = None, None, 0.0, 0.0, time.time()

    def status(state, **extra):
        (here / "status.json").write_text(json.dumps(dict(state=state, current=current, load_s=round(load_s, 1),
                                                          at=time.strftime("%H:%M:%S"), **extra)))

    status("idle")
    while True:
        requests = pending_requests()
        if not requests:
            if time.time() - idle_since > IDLE_EXIT_S:
                break
            time.sleep(0.3)
            continue
        idle_since = time.time()

        mine = [r for r in requests if r[1]["thin"] == current]
        if not mine:
            # Others are waiting. Hold the open binary a little: its next request is
            # usually seconds away, and letting it go costs a full re-analysis.
            linger = min(900.0, max(20.0, load_s * 1.5))
            if current and time.time() - last_served < linger:
                time.sleep(0.5)
                continue
            path, request = requests[0]
            if rea:
                rea.close()
            rea, current = Rea(), request["thin"]
            ok, text = rea.tool("open_binary", {"path": current, "provider_id": "hopper"})
            log(f"open {current}: {'ok' if ok else text[:300]}")
            status("analysing", binary=request.get("binary"))
            started = time.time()
            ok, text = rea.tool("list_documents", {})
            first = request["targets"][0]
            ok, text = rea.tool("procedure_pseudo_code", {"procedure": first})
            load_s = time.time() - started
            log(f"analysis ready in {load_s:.0f}s")
            mine = [(path, request)]

        # Smallest first: a caller waiting on six procedures must not sit behind a
        # pre-warm of thousands.
        path, request = min(mine, key=lambda r: len(r[1]["targets"]))
        status("serving", binary=request.get("binary"), queued=len(requests))
        done = 0
        for target in request["targets"]:
            out = target_file(request["key"], target)
            if out.exists():
                continue
            try:
                ok, text = rea.tool("procedure_pseudo_code", {"procedure": target})
            except RuntimeError as exc:
                log(f"rea died: {exc}; restarting on next request")
                rea, current = None, None
                break
            body = ""
            if ok:
                try:
                    body = json.loads(text).get("normalized_result") or ""
                except ValueError:
                    body = ""
            if body.strip():
                tmp = out.with_suffix(f".tmp{os.getpid()}")
                tmp.write_text(body)
                tmp.replace(out)
                done += 1
            else:
                out.with_suffix(".err").write_text(text[:4000])
        else:
            path.unlink(missing_ok=True)
            log(f"served {done}/{len(request['targets'])} for {request.get('binary')}")
        last_served = time.time()
        status("idle")

    if rea:
        rea.close()
    (here / "pid").unlink(missing_ok=True)
    log("daemon exit")


# ---------------------------------------------------------------- client

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary", nargs="?")
    ap.add_argument("targets", nargs="*")
    ap.add_argument("--match")
    ap.add_argument("--max", type=int, default=400, help="most symbols --match may queue")
    ap.add_argument("--no-wait", action="store_true")
    ap.add_argument("--retry", action="store_true", help="ask again for targets that failed before")
    ap.add_argument("--serve", action="store_true")
    ap.add_argument("--status", action="store_true")
    args = ap.parse_args()

    if args.serve:
        return serve()
    if args.status:
        pid = daemon_pid()
        print(f"daemon: {'pid ' + str(pid) if pid else 'not running'}")
        try:
            print((root() / "status.json").read_text())
        except OSError:
            pass
        for path, request in pending_requests():
            print(f"queued\t{len(request['targets'])} targets\t{request.get('binary')}\t{path.name}")
        return
    if not args.binary:
        ap.error("binary required")

    binary = Path(args.binary)
    thin = disasm.thin_slice(binary)
    key = disasm.cache_key(binary)
    names = {}
    targets = [t.lower() if t.lower().startswith("0x") else t for t in args.targets]
    if args.match:
        import re
        code = disasm.code_symbols(disasm.symbol_index(binary))
        rx = re.compile(args.match, re.I)
        hits = [(a, n) for a, n in code if rx.search(n)]
        if len(hits) > args.max:
            sys.exit(f"--match hits {len(hits)} symbols, more than --max {args.max}; narrow the regex or raise --max")
        for address, name in hits:
            targets.append(f"{address:#x}")
            names[f"{address:#x}"] = name
    if not targets:
        ap.error("no targets")

    def state(target):
        out = target_file(key, target)
        if out.exists():
            return "ok"
        if out.with_suffix(".err").exists():
            return "err"
        return "todo"

    if args.retry:
        for target in targets:
            target_file(key, target).with_suffix(".err").unlink(missing_ok=True)

    todo = [t for t in targets if state(t) == "todo"]
    if todo:
        ensure_daemon()
        request = dict(binary=str(binary), thin=str(thin), key=key, targets=todo)
        name = f"{time.time():.3f}-{uuid.uuid4().hex[:8]}.json"
        tmp = root() / f"{name}.tmp"
        tmp.write_text(json.dumps(request))
        tmp.replace(root() / "req" / name)
        if args.no_wait:
            print(f"queued {len(todo)} targets for {binary.name}", file=sys.stderr)
            return
        waited = 0.0
        while any(state(t) == "todo" for t in todo):
            if not daemon_pid():
                sys.exit("hopper.py: the daemon died; see hopperd/daemon.log, then retry")
            time.sleep(0.5)
            waited += 0.5
            if waited % 300 == 0:
                left = sum(1 for t in todo if state(t) == "todo")
                print(f"hopper.py: {left} targets still pending after {waited:.0f}s "
                      f"(Hopper may be analysing a large binary; --status shows it)", file=sys.stderr)

    for target in targets:
        label = f"{target}  {names.get(target, '')}".rstrip()
        out = target_file(key, target)
        if out.exists():
            print(f"// ===== {label}\n{out.read_text()}\n")
        else:
            err = out.with_suffix(".err")
            print(f"// ===== {label}\n// FAILED: {err.read_text()[:600] if err.exists() else 'no result'}\n")


if __name__ == "__main__":
    main()
