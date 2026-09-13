#!/usr/bin/env python3
"""Map delivery recovery after a dongle outage. usage: drought2.py <label> <held|rested> <L> <offset s ...>
held  : key -> link held -> rail off L s -> rail on -> keys at the given offsets after rail-on
rested: controller already resting (no key) -> rail off L s -> rail on -> keys at the offsets"""
import json, os, socket, subprocess, sys, time
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
def net(sec): d=ppk({"cmd":"stats","last":sec}); return d["mean_mA"]-FLOOR_NOM
def emit(line): print(line,flush=True); open(LOG,"a").write(line+"\n")
def dongle():
    try:
        out=subprocess.run([D,"--status"],capture_output=True,text=True,timeout=6).stdout.strip().splitlines(); out=out[-1] if out else ""
        return "C" if "connection=connected" in out else ("W" if "waiting" in out else "-")
    except Exception: return "-"
def rail(on):
    """Dongle rail on/off through its WCH-Link; raises on failure so a key result is never
    reported for an outage that did not happen."""
    subprocess.run([MC,"-k3" if on else "-kt","-C","linke","-l",DONGLE_PROBE],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=20,check=True)
def nucleo():
    try:
        out=subprocess.run(["python3",BD+"/bench.py","--elf",E,"status"],capture_output=True,text=True,timeout=30).stdout
        return " ".join(l.split()[-1] for l in out.splitlines() if l.startswith(("link","last_status","module")))
    except Exception: return "?"
class Tapper:
    def __init__(self):
        self.ocd=bench.OpenOCD(bench.DEFAULT_CFG); self.ocd.connect(); self.sym=bench.load_symbols(E)
        self.ocd.write_byte(self.sym["bench_tap_dur_ms"],60); self.ocd.write_byte(self.sym["bench_tap_dur_ms"]+1,0)
    def arm(self,delay_ms=200):
        self.ocd.write_word(self.sym["bench_tap_delay_ms"],delay_ms); return time.monotonic()*1000.0+delay_ms
    def close(self): self.ocd.sock.close()
def key(tp,tag):
    out=f"{S}/dr2_{tag}.ndjson"
    try: os.remove(out)
    except FileNotFoundError: pass
    cap=subprocess.Popen([VPY,T,"capture","--keys","105","--seconds","4","--out",out,"--quiet"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    time.sleep(1.2); tk=tp.arm(); cap.wait(timeout=12)
    ev=sorted((json.loads(l)["t_ms"],json.loads(l)["ev"]) for l in open(out)) if os.path.exists(out) else []
    downs=[t for t,e in ev if e=="down"]; ups=[t for t,e in ev if e=="up"]
    return ("OK %.0fms"%(downs[0]-tk)) if (len(downs)==1 and len(ups)==1 and downs[0]<ups[0]) else f"FAIL(d{len(downs)}u{len(ups)})"
if __name__=="__main__":
    label,mode,L=sys.argv[1],sys.argv[2],float(sys.argv[3]); offs=[float(x) for x in sys.argv[4:]]
    tp=Tapper()
    try:
        # settle
        t0=time.time()
        while net(5)>=0.3 and time.time()-t0<150: time.sleep(5)
        if mode=="held": tp.arm(); time.sleep(1.2)
        emit(f"{label} {mode} L={L}s: pre dongle={dongle()} net={net(2):.1f}mA nucleo[{nucleo()}]")
        rail(False); time.sleep(L); rail(True); t_on=time.time()
        for o in offs:
            time.sleep(max(0.0,t_on+o-1.4-time.time()))
            r=key(tp,f"{label}_{mode}_{o:.0f}"); dg=dongle()
            emit(f"{label} {mode} L={L}s key@+{o:.0f}s: {r}  dongle={dg} net={net(1):.1f}mA nucleo[{nucleo()}]")
    finally: tp.close()
