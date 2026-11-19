#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""End-to-end check of `victus debug-adapter` (ADR-0099): drives the Debug Adapter Protocol over
stdio like an editor would. Usage: dap_smoke.py <victus executable> <work folder>"""
import json
import os
import shutil
import subprocess
import sys
import threading

exe, work = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)
project = os.path.join(work, "game")
subprocess.run([exe, "new", project, "--template", "3d"], check=True, stdout=subprocess.DEVNULL)
crate = os.path.join(project, "assets", "scripts", "crate.luau")
assert "self.speed = vector.magnitude" in open(crate).read().splitlines()[13], "template changed: update line 14"

proc = subprocess.Popen([exe, "debug-adapter"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, cwd=work)
watchdog = threading.Timer(240, proc.kill)
watchdog.start()
seq = 0
log = []


def send(command, arguments=None):
    global seq
    seq += 1
    body = json.dumps({"seq": seq, "type": "request", "command": command, "arguments": arguments or {}}).encode()
    proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    proc.stdin.flush()
    return seq


def receive():
    header = b""
    while not header.endswith(b"\r\n\r\n"):
        c = proc.stdout.read(1)
        if not c:
            sys.exit("adapter closed stdout; messages so far:\n" + "\n".join(log))
        header += c
    assert header.startswith(b"Content-Length: "), header  # nothing but protocol on stdout
    message = json.loads(proc.stdout.read(int(header.split(b":")[1].split(b"\r")[0])))
    log.append(json.dumps(message)[:400])
    return message


def response(s):
    while True:
        m = receive()
        if m["type"] == "response" and m["request_seq"] == s:
            return m


def event(name):
    while True:
        m = receive()
        if m["type"] == "event" and m["event"] == name:
            return m


def check(cond, what):
    if not cond:
        proc.kill()
        sys.exit("FAILED: " + what + "\n" + "\n".join(log[-20:]))


r = response(send("initialize", {"adapterID": "victus", "linesStartAt1": True}))
check(r["success"] and r["body"]["supportsLogPoints"], "initialize capabilities")
r = response(send("launch", {"project": "missing-folder"}))
check(not r["success"], "launch of a missing project fails")
r = response(send("launch", {"project": project, "frames": 90}))
check(r["success"], "launch")
event("initialized")
r = response(send("setBreakpoints", {"source": {"path": crate}, "breakpoints": [{"line": 14}, {"line": 18, "logMessage": "never"}]}))
check(len(r["body"]["breakpoints"]) == 2, "two breakpoint results")
response(send("setExceptionBreakpoints", {"filters": ["uncaught"]}))
response(send("configurationDone"))

e = event("stopped")
check(e["body"]["reason"] == "breakpoint", "stopped at the breakpoint")
st = response(send("stackTrace", {"threadId": 1}))["body"]["stackFrames"]
check(st[0]["line"] == 14 and st[0]["name"] == "update", "top frame is Crate:update line 14")
check(os.path.samefile(st[0]["source"]["path"], crate), "frame source maps back to the file")
scopes = response(send("scopes", {"frameId": st[0]["id"]}))["body"]["scopes"]
local_vars = response(send("variables", {"variablesReference": scopes[0]["variablesReference"]}))["body"]["variables"]
names = [v["name"] for v in local_vars]
check(names[:2] == ["self", "dt"], "locals self, dt: %s" % names)
fields = response(send("variables", {"variablesReference": local_vars[0]["variablesReference"]}))["body"]["variables"]
check("speed" in [f["name"] for f in fields], "self expands to its fields")
r = response(send("evaluate", {"expression": "math.floor(dt * 600 + 0.5)", "frameId": st[0]["id"]}))
check(r["success"] and r["body"]["result"] == "10", "evaluate in the frame: %s" % r)
r = response(send("setVariable", {"variablesReference": scopes[0]["variablesReference"], "name": "dt", "value": "0.5"}))
check(r["success"] and r["body"]["value"] == "0.5", "setVariable")

response(send("next", {"threadId": 1}))
e = event("stopped")
check(e["body"]["reason"] == "step", "step stop")
st = response(send("stackTrace", {"threadId": 1}))["body"]["stackFrames"]
check(st[0]["line"] == 15, "stepped to line 15")

response(send("setBreakpoints", {"source": {"path": crate}, "breakpoints": []}))
response(send("continue", {"threadId": 1}))
response(send("pause", {"threadId": 1}))
e = event("stopped")
check(e["body"]["reason"] == "pause", "pause stops at the next script line")
r = response(send("evaluate", {"expression": "1 + 1", "context": "repl", "frameId": 1}))
check(r["success"], "evaluate while paused")
response(send("continue", {"threadId": 1}))
event("exited")
event("terminated")
proc.stdin.close()
code = proc.wait(timeout=30)
watchdog.cancel()
check(code == 0, "adapter exit code %s" % code)
print("dap smoke: OK (%d messages)" % len(log))
