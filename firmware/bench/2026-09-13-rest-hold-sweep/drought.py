#!/usr/bin/env python3
"""Poll-drought ladder: cut the DONGLE's rail for L seconds while the controller holds a live link
(1 s into the post-key hold), watch the controller's power path, the dongle's return, the reconnect,
the next key's delivery/latency, and the episode charge. usage: drought.py <label> <H> [L ...]"""
import json, os, signal, socket, subprocess, sys, time, statistics as st
# Bench environment (override with environment variables; defaults are the 2026-09-13 bench):
#   OPENKEYBOARD_QMK   qmk_firmware checkout with keyboards/handwired/opencontroller_bench
#   OPENDONGLE_TOOL    the `opendongle` host tool binary
#   MINICHLINK         minichlink binary (WCH-Link probe control)
#   DONGLE_PROBE       WCH-Link serial powering/attached to the dongle (rail control)
#   PPK2D_SOCK         ppk2d unix socket (PPK2 inline on the controller rail)
#   HID_CAPTURE        firmware/bench/tools/hid_capture.py of this repo
#   BENCH_PY           python with pyobjc-Quartz for the CGEventTap oracle
S=os.path.dirname(os.path.abspath(__file__))
Q=os.environ.get("OPENKEYBOARD_QMK", os.path.expanduser("~/Development/openkeyboard/qmk_firmware")); BD=Q+"/keyboards/handwired/opencontroller_bench"
E=Q+"/.build/handwired_opencontroller_bench_oracle.elf"
REPO=os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
T=os.environ.get("HID_CAPTURE", os.path.join(REPO, "firmware", "bench", "tools", "hid_capture.py"))
VPY=os.environ.get("BENCH_PY", os.path.join(os.path.dirname(os.path.abspath(__file__)), "vpy", "bin", "python"))
D=os.environ.get("OPENDONGLE_TOOL", os.path.expanduser("~/Development/openkeyboard/OpenDongle/tools/target/release/opendongle"))
SOCK=os.environ.get("PPK2D_SOCK", os.path.expanduser("~/.ppk2d.sock"))
MC=os.environ.get("MINICHLINK", os.path.expanduser("~/Development/WCH/ch32fun/minichlink/minichlink")); DONGLE_PROBE=os.environ.get("DONGLE_PROBE", "CEBD8F0653EF")
FLOOR_NOM=float(os.environ.get("BENCH_FLOOR_MA", "1.55")); LOG=S+"/drought.log"
sys.path.insert(0,BD); import bench
def ppk(req):
    s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.connect(SOCK); s.sendall((json.dumps(req)+"\n").encode())
    r=json.loads(s.makefile("r").readline()); s.close(); return r
def stats_last(sec): return ppk({"cmd":"stats","last":sec})
def stats_marks(a,b): return ppk({"cmd":"stats","since":a,"until":b})
def mark(l): ppk({"cmd":"mark","label":l}); return time.time()
def p50(d): return d.get("p50_mA", d.get("p50_mA_1ms"))
def emit(line): print(line,flush=True); open(LOG,"a").write(line+"\n")
def dongle():
    try:
        out=subprocess.run([D,"--status"],capture_output=True,text=True,timeout=6).stdout.strip().splitlines()
        out=out[-1] if out else ""
        return "C" if "connection=connected" in out else ("W" if "waiting" in out else "-")
    except Exception: return "-"
def rail(on):
    """Dongle rail on/off through its WCH-Link; raises on failure so an episode never reports an
    outage that did not happen."""
    subprocess.run([MC,"-k3" if on else "-kt","-C","linke","-l",DONGLE_PROBE],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=20,check=True)
def nucleo():
    try:
        out=subprocess.run(["python3",BD+"/bench.py","--elf",E,"status"],capture_output=True,text=True,timeout=30).stdout
        return " ".join(l.split()[-1] for l in out.splitlines() if l.startswith(("link","last_status","module")))
    except Exception: return "?"
def diag():
    try:
        out=subprocess.run(["python3",BD+"/bench.py","--elf",E,"diag"],capture_output=True,text=True,timeout=40).stdout
        return {p[0]:p[1] for p in (l.split() for l in out.splitlines()) if len(p)==2}
    except Exception: return {}
def cls(d): return "S" if (d["mean_mA"]-FLOOR_NOM<0.3 and p50(d)<2.0) else "R"
def wait_settled(cap=180):
    t0=time.time()
    while time.time()-t0<cap:
        d=stats_last(5)
        if cls(d)=="S": return True
        time.sleep(5)
    emit(f"  WARN: not settled after {cap}s"); return False
class Tapper:
    def __init__(self):
        self.ocd=bench.OpenOCD(bench.DEFAULT_CFG); self.ocd.connect(); self.sym=bench.load_symbols(E)
        self.ocd.write_byte(self.sym["bench_tap_dur_ms"],60); self.ocd.write_byte(self.sym["bench_tap_dur_ms"]+1,0)
    def arm(self,delay_ms=200):
        self.ocd.write_word(self.sym["bench_tap_delay_ms"],delay_ms); return time.monotonic()*1000.0+delay_ms
    def close(self): self.ocd.sock.close()
def episode(label,H,L,tp):
    if not wait_settled():
        emit(f"{label} L={L}s: SKIPPED, not settled (the baseline and the outage would include residual activity)"); return
    d0=diag(); drop0=int(d0["ll_drop"]) if "ll_drop" in d0 else None
    floor=stats_last(5).get("p05_mA",stats_last(5).get("p05_mA_1ms"))
    tp.arm(); time.sleep(1.2)                       # key -> link CONNECTED, hold running; drought starts ~1 s in
    t_off=mark(f"{label}_L{L}_off"); rail(False); t_off_done=time.time()   # power is off once the command returns
    trace=[]; t0=time.time()
    while time.time()-t0<L: time.sleep(1); trace.append(stats_last(1)["mean_mA"]-floor)
    t_on_start=time.time(); rail(True); t_on=mark(f"{label}_L{L}_on")
    actual=f"{t_on_start-t_off_done:.1f}-{t_on-t_off:.1f}s"   # outage bounds: command-return to command-start, and command-start to command-return
    back=None; t1=time.time()
    while time.time()-t1<30:
        if dongle()!="-": back=time.time()-t_on; break
        time.sleep(0.5)
    post=[]; t2=time.time(); dg="-"
    while time.time()-t2<12: time.sleep(1); post.append(stats_last(1)["mean_mA"]-floor)   # what the controller does once the dongle is back
    dg=dongle()
    # next key: delivered? latency?
    out=f"{S}/dr_{label}_L{L}.ndjson"
    try: os.remove(out)
    except FileNotFoundError: pass
    cap=subprocess.Popen([VPY,T,"capture","--keys","105","--seconds","6","--out",out,"--quiet"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    time.sleep(1.5); tk=tp.arm(); cap.wait(timeout=15)
    ev=sorted((json.loads(l)["t_ms"],json.loads(l)["ev"]) for l in open(out)) if os.path.exists(out) else []
    downs=[t for t,e in ev if e=="down"]; ups=[t for t,e in ev if e=="up"]
    key="OK lat=%.0fms"%(downs[0]-tk) if (len(downs)==1 and len(ups)==1 and downs[0]<ups[0]) else f"FAIL(downs={len(downs)} ups={len(ups)})"
    # lock
    consec=0; t_lock=None; t3=time.time()
    while time.time()-t3<180:
        time.sleep(5); consec=consec+1 if cls(stats_last(5))=="S" else 0
        if consec>=2: t_lock=time.time()-10; break
    mark(f"{label}_L{L}_lock"); time.sleep(0.5)
    d1=diag(); drop1=int(d1["ll_drop"]) if "ll_drop" in d1 else None
    path="UNKNOWN(no diag)" if drop0 is None or drop1 is None else ("SUPERVISION" if drop1>drop0 else "REST")
    a=stats_marks(f"{label}_L{L}_off",f"{label}_L{L}_lock"); mC=(a["mean_mA"]-floor)*a["seconds"]
    fmt=lambda v:" ".join(f"{x:.1f}" for x in v)
    emit(f"{label} L={L}s (actual {actual}): path={path} (ll_drop {drop0}->{drop1}) | during outage net mA/s: {fmt(trace)} | dongle back {'never' if back is None else '%.1fs'%back} after rail-on | after return net mA/s: {fmt(post)} dongle={dg} nucleo[{nucleo()}] | next key {key} | lock {'CENSORED' if t_lock is None else '%.0fs after rail-on'%(t_lock-t_on)} | episode {a['seconds']:.0f}s {mC:.0f} mC")
if __name__=="__main__":
    label=sys.argv[1]; H=float(sys.argv[2]); Ls=[float(x) for x in sys.argv[3:]] or [1.5,2.5,4,8,30]
    emit(f"=== drought ladder {label}: H={H}s L={Ls}  dongle={dongle()}")
    tp=Tapper()
    try:
        for L in Ls: episode(label,H,L,tp); time.sleep(10)
    finally: tp.close()
    emit(f"=== {label} done")
