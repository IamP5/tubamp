import json, subprocess, sys, time, os, glob
rea = sorted(glob.glob(os.path.expanduser("~/.npm/_npx/*/node_modules/.bin/rea")), key=os.path.getmtime)[-1]
env = dict(os.environ, REA_HOPPER_STARTUP_TIMEOUT_MS="5400000")
p = subprocess.Popen([rea, "--mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(os.environ["RE_STATE"]+"/mcp_probe.err","w"), text=True, bufsize=1, env=env, cwd=os.environ["RE_STATE"])
nid = 0
def call(method, params=None, notify=False):
    global nid
    msg = {"jsonrpc":"2.0","method":method}
    if params is not None: msg["params"]=params
    if not notify:
        nid += 1; msg["id"]=nid
    p.stdin.write(json.dumps(msg)+"\n"); p.stdin.flush()
    if notify: return None
    while True:
        line = p.stdout.readline()
        if not line: raise SystemExit("server closed")
        try: o = json.loads(line)
        except Exception: continue
        if o.get("id")==nid: return o
def tool(name, args):
    t=time.time()
    r = call("tools/call", {"name":name,"arguments":args})
    dt=time.time()-t
    res = r.get("result", r)
    text = "".join(c.get("text","") for c in res.get("content",[])) if isinstance(res, dict) else str(res)
    print(f"-- {name} {args} : {dt:.1f}s isError={res.get('isError') if isinstance(res,dict) else None} len={len(text)}")
    return text
call("initialize", {"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"probe","version":"0"}})
call("notifications/initialized", {}, notify=True)
thin = glob.glob(os.environ["RE_STATE"]+"/thin/*-Driver")[0]
print(tool("open_binary", {"path": thin, "provider_id":"hopper"})[:600])
for a in ["0x19980","0x1c620","0x19b68"]:
    t = tool("procedure_pseudo_code", {"procedure": a})
    print(t[:300].replace("\n"," | "))
print(tool("procedure_assembly", {"procedure": "0x19980"})[:300])
print(tool("close_binary", {})[:300])
p.stdin.close(); p.terminate()
