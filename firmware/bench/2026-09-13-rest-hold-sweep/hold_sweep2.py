#!/usr/bin/env python3
"""KBD_REST_IDLE_TICKS hold sweep, v2 (after the Codex protocol review).
C: key-from-settled-rest episodes, full tail captured (reconnect, hold, rest entry, lock), bracketed floors,
   resting counterfactual -> total and excess mC.  D: continuous host capture, strict one-ordered-pair scoring,
   jittered rest depths (far ~6 s, near ~0.9 s).  W: seeded typing-cadence workload (identical across H):
   total mC over the workload until lock, delivered count, latency split by reconnect vs held keys.
usage: hold_sweep2.py <label> <hold_s> [--image BIN] [--charge 6] [--far 24] [--near 12] [--work 1] [--seed 7]"""
import argparse, csv, hashlib, json, os, random, signal, socket, subprocess, sys, threading, time, statistics as st
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
FS=100000; FLOOR_NOM=float(os.environ.get("BENCH_FLOOR_MA", "1.55")); LOG=S+"/hold_sweep.log"; THR_NET=1500.0
sys.path.insert(0,BD); import bench

def ppk(req):
    s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.connect(SOCK); s.sendall((json.dumps(req)+"\n").encode())
    r=json.loads(s.makefile("r").readline()); s.close(); return r
def stats_last(sec): return ppk({"cmd":"stats","last":sec})
def stats_marks(a,b=None): return ppk({"cmd":"stats","since":a,"until":b} if b else {"cmd":"stats","since":a})
def mark(l): ppk({"cmd":"mark","label":l}); return time.time()
def p50(d): return d.get("p50_mA", d.get("p50_mA_1ms"))
def p05(d): return d.get("p05_mA", d.get("p05_mA_1ms"))
def emit(line): print(line,flush=True); open(LOG,"a").write(line+"\n")
def dongle():
    try:
        out=subprocess.run([D,"--status"],capture_output=True,text=True,timeout=10).stdout.strip().splitlines()[-1]
        return out[out.find("connection="):].replace("connection=","").replace("  "," ").strip()
    except Exception as e: return "?"
def diag_snapshot(tag):
    try:
        out=subprocess.run(["python3",BD+"/bench.py","--elf",E,"diag"],capture_output=True,text=True,timeout=40).stdout
        keep={}
        for line in out.splitlines():
            parts=line.split()
            if len(parts)==2 and parts[0] in ("rf_state","ll_boot","boot_reset","fault_marker","sleep_entered","wake_gpio","ll_hid_rx_down","ll_hid_tx_down","ll_hid_tx_done_down","ll_hid_fifo_drop","ll_drop"): keep[parts[0]]=parts[1]
        emit(f"  diag[{tag}]: "+(" ".join(f"{k}={v}" for k,v in keep.items()) if keep else out.strip().splitlines()[-1] if out.strip() else "no output"))
    except Exception as e: emit(f"  diag[{tag}]: failed {e!r}")
def cls(d): return "S" if (d["mean_mA"]-FLOOR_NOM<0.3 and p50(d)<2.0) else "R"
class NotSettled(Exception): pass
def wait_settled(cap=150):
    """Block until a 5 s window is settled (net < 0.3 mA, p50 < 2 mA). Raises NotSettled after
    `cap` seconds: an episode must start from certified rest or it is not recorded."""
    t0=time.time()
    while True:
        d=stats_last(5)
        if cls(d)=="S": return d, time.time()-t0
        if time.time()-t0>cap: raise NotSettled(f"not settled after {cap}s (net={d['mean_mA']-FLOOR_NOM:.2f} p50={p50(d):.2f})")
        time.sleep(5)
class Tapper:
    def __init__(self):
        self.ocd=bench.OpenOCD(bench.DEFAULT_CFG); self.ocd.connect(); self.sym=bench.load_symbols(E)
        self.ocd.write_byte(self.sym["bench_tap_dur_ms"],60); self.ocd.write_byte(self.sym["bench_tap_dur_ms"]+1,0)
    def arm(self,delay_ms=200):
        self.ocd.write_word(self.sym["bench_tap_delay_ms"],delay_ms)
        tm=time.monotonic()*1000.0+delay_ms; tw=time.time()+delay_ms/1000.0; return tm,tw
    def close(self): self.ocd.sock.close()
def read_events(path):
    ev=[]
    if os.path.exists(path):
        for line in open(path):
            try: r=json.loads(line); ev.append((r["t_ms"],r["ev"]))
            except Exception: pass
    return sorted(ev)
def score_all(ev,tkeys,slot_ms):
    """Return (per-key (result, latency, n), stray). Sequential pairing if the stream is exactly one
    ordered down/up per key, each down after its key and before the next key; otherwise per-key
    windows. `stray` counts captured events attributed to no key -- a non-zero stray voids the
    'exactly one ordered down/up' claim for the phase (Codex review)."""
    pairs=[]; i=0
    while i<len(ev)-1:
        if ev[i][1]=="down" and ev[i+1][1]=="up": pairs.append((ev[i][0],ev[i+1][0])); i+=2
        else: i+=1
    nxt=tkeys[1:]+[tkeys[-1]+slot_ms]
    if len(pairs)==len(tkeys) and len(ev)==2*len(tkeys) and all(tk-50.0<=d<tn-50.0 for (d,u),tk,tn in zip(pairs,tkeys,nxt)):
        return [("OK",d-tk,2) for (d,u),tk in zip(pairs,tkeys)],0
    out=[]; attributed=0
    for k,tk in enumerate(tkeys):
        t_next=tkeys[k+1] if k+1<len(tkeys) else tk+slot_ms
        r=score(ev,tk,t_next); out.append(r); attributed+=r[2]
    return out,len(ev)-attributed
def score(ev,t_key,t_next):
    w=[(t,e) for t,e in ev if t_key-50.0<=t<t_next-50.0]
    downs=[t for t,e in w if e=="down"]; ups=[t for t,e in w if e=="up"]
    if len(downs)==1 and len(ups)==1 and downs[0]<ups[0]: return "OK", downs[0]-t_key, len(w)
    if not w: return "LOST", None, 0
    if downs and not ups: return "STUCK", downs[0]-t_key, len(w)
    if len(downs)>1 or len(ups)>1: return "EXTRA", (downs[0]-t_key if downs else None), len(w)
    return "ODD", (downs[0]-t_key if downs else None), len(w)
def lat_summary(l):
    if not l: return "none"
    l=sorted(l); return f"med={st.median(l):.0f} p90={l[int(0.9*(len(l)-1))]:.0f} max={l[-1]:.0f} min={l[0]:.0f} ms (n={len(l)})"
def analyse(path,t0_unix,t_key,floor_uA,rest_rate_mA,tail_s):
    i=[float(r[1]) for r in list(csv.reader(open(path)))[1:]]
    k=int((t_key-t0_unix)*FS); k=max(0,min(k,len(i)-1)); pre=i[:k]; post=i[k:]
    fp=sorted(pre)[len(pre)//20]/1000.0 if len(pre)>100 else None
    fq=sorted(post[-3*FS:])[len(post[-3*FS:])//20]/1000.0
    total=sum(v-floor_uA for v in post)/FS/1000.0; span=len(post)/FS; cf=rest_rate_mA*span; thr=floor_uA+THR_NET
    runs=[]; s=None
    for j,v in enumerate(post):
        if v>thr and s is None: s=j
        elif v<=thr and s is not None:
            if j-s>20: runs.append((s,j))
            s=None
    if s is not None and len(post)-s>20: runs.append((s,len(post)))
    longest=max(runs,key=lambda r:r[1]-r[0]) if runs else (0,0)
    above=sum((b-a) for a,b in runs)/FS
    # lock: first whole second after rest entry from which every later second stays < floor+0.3 mA net, certified >= 5 s
    rest_entry=longest[1]/FS; secs=[]
    for a in range(int(rest_entry)+1, int(span)):
        seg=post[a*FS:(a+1)*FS]; secs.append((a, sum(seg)/len(seg)/1000.0-floor_uA/1000.0))
    lock=None
    for idx,(a,net) in enumerate(secs):
        if all(n<0.3 for _,n in secs[idx:]) and (span-a)>=5.0: lock=a; break
    return dict(pre_floor=fp, post_floor=fq, total=total, cf=cf, excess=total-cf, recon=longest[0]/FS, hold=(longest[1]-longest[0])/FS,
                rest_entry=rest_entry, above=above, bursts=len(runs), peak=max(post)/1000.0, lock=lock, span=span, sec_tail=secs[-6:])
def phase_charge(label,H,n,tp):
    rows=[]
    for ep in range(1,n+1):
        try: d,waited=wait_settled()
        except NotSettled as e: emit(f"{label} C{ep}: SKIPPED, {e}"); continue
        floor=p05(d)*1000.0; rest_rate=d["mean_mA"]-p05(d)
        mark(f"{label}_c{ep}"); tm,tw=tp.arm(); tail=H+13.0
        time.sleep(max(0.0,(tw+tail)-time.time())); out=f"{S}/hs2_{label}_c{ep}.csv"; r=ppk({"cmd":"raw","seconds":tail+1.5,"out":out})
        a=analyse(out,r["t0_unix"],tw,floor,rest_rate,tail)
        emit(f"{label} C{ep}: waited={waited:.0f}s floor={floor/1000:.3f}/{a['post_floor']:.3f}mA rest_rate={rest_rate*1000:.0f}uA | recon={a['recon']*1000:.0f}ms hold={a['hold']:.2f}s rest_entry={a['rest_entry']:.2f}s above_thr={a['above']:.2f}s bursts={a['bursts']} peak={a['peak']:.1f}mA | total={a['total']:.1f}mC cf={a['cf']:.1f} EXCESS={a['excess']:.1f}mC | lock@{'CENSORED' if a['lock'] is None else str(a['lock'])+'s'} (tail {a['span']:.0f}s) dongle={dongle()}")
        rows.append(a); time.sleep(10)
    if not rows:
        emit(f"{label} CHARGE: 0 of {n} episodes recorded (none started from certified rest)"); return
    ex=[r["excess"] for r in rows]; hb=[r["hold"] for r in rows]; lk=[r["lock"] for r in rows if r["lock"] is not None]; rc=[r["recon"]*1000 for r in rows]
    emit(f"{label} CHARGE n={len(rows)} of {n}: excess mC mean={st.mean(ex):.1f} min={min(ex):.1f} max={max(ex):.1f} sd={st.pstdev(ex):.2f} | hold mean={st.mean(hb):.2f}s | recon ms med={st.median(rc):.0f} max={max(rc):.0f} | lock s: {('med=%.0f max=%.0f'%(st.median(lk),max(lk))) if lk else 'none'} certified {len(lk)}/{len(rows)}")
def phase_deliv(label,H,n,base_rest,jit,sub,tp,rng):
    wait_settled()
    rests=[max(0.0,base_rest+jit(rng)) for _ in range(n)]
    out=f"{S}/hs2_{label}_{sub}.ndjson"
    try: os.remove(out)
    except FileNotFoundError: pass
    cap=subprocess.Popen([VPY,T,"capture","--keys","105","--seconds",str(int(sum(rests)+3.0*n+20)),"--out",out,"--quiet"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    time.sleep(1.5); keys=[]
    for t in range(n):
        time.sleep(rests[t]); slot=time.time(); tm,tw=tp.arm(); keys.append((tm,tw,rests[t])); time.sleep(max(0.0,slot+3.0-time.time()))
    time.sleep(2.0); cap.send_signal(signal.SIGINT)
    try: cap.wait(timeout=10)
    except subprocess.TimeoutExpired: cap.kill()
    ev=read_events(out); ok=0; lats=[]; bad=[]
    scored,stray=score_all(ev,[k[0] for k in keys],3000.0)
    if stray: bad.append(f"STRAY(n={stray})")
    for t,(tm,tw,rest) in enumerate(keys):
        res,lat,nev=scored[t]; inrest=rest+3.0-H-0.06
        if res=="OK": ok+=1; lats.append(lat)
        else: bad.append(f"{sub}{t+1}:{res}(n={nev})")
        emit(f"{label} D-{sub}{t+1}: in_rest~{inrest:.2f}s -> {res}" + (f" lat={lat:.0f}ms" if lat is not None else ""))
    emit(f"{label} DELIV-{sub}: {ok}/{n} exactly one ordered down/up; failures: {' '.join(bad) if bad else 'none'} | events total={len(ev)} | latency {lat_summary(lats)}")
def phase_work(label,H,seed,tp):
    rng=random.Random(seed); gaps=[]
    for _ in range(60):
        u=rng.random(); gaps.append(0.3+0.4*rng.random() if u<0.5 else (1.0+3.0*rng.random() if u<0.8 else 5.0+7.0*rng.random()))
    recon_keys=[0]+[k for k in range(1,60) if gaps[k]>H+0.12]   # key 0 starts from settled rest; gaps >= 0.3 s: arm(100 ms) never lands inside the previous 60 ms press (bench.c:175-184)   # key k follows a gap longer than the hold -> lands on a reconnect
    d,waited=wait_settled(); floor=p05(d); rest_rate=d["mean_mA"]-floor
    out=f"{S}/hs2_{label}_w.ndjson"
    try: os.remove(out)
    except FileNotFoundError: pass
    cap=subprocess.Popen([VPY,T,"capture","--keys","105","--seconds",str(int(sum(gaps)+200)),"--out",out,"--quiet"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    time.sleep(1.5); t_w0=mark(f"{label}_w0"); base=time.time()+0.5; keys=[]; tk=base
    for k in range(60):
        tk=tk+(gaps[k] if k>0 else 0.0); time.sleep(max(0.0,tk-0.1-time.time())); tm,tw=tp.arm(100); keys.append((tm,tw))
    time.sleep(0.6)                      # the last key fires at +100 ms and lands within ~100 ms: keep it inside the active window
    t_wend=mark(f"{label}_wend")
    consec=0; t_lock=None; t0=time.time()
    while time.time()-t0<150:
        time.sleep(5); consec=consec+1 if cls(stats_last(5))=="S" else 0
        if consec>=2: t_lock=time.time()-10; break
    if t_lock is None: t_lock=time.time(); censored=True
    else: censored=False
    mark(f"{label}_wlock"); time.sleep(0.5)
    a=stats_marks(f"{label}_w0",f"{label}_wend"); b=stats_marks(f"{label}_w0",f"{label}_wlock")
    act_s=a["seconds"]; all_s=b["seconds"]; act_mC=(a["mean_mA"]-floor)*act_s; all_mC=(b["mean_mA"]-floor)*all_s; cf=rest_rate*all_s
    cap.send_signal(signal.SIGINT)
    try: cap.wait(timeout=10)
    except subprocess.TimeoutExpired: cap.kill()
    ev=read_events(out); ok=0; lat_r=[]; lat_h=[]; bad=[]
    scored,stray=score_all(ev,[k[0] for k in keys],5000.0)
    if stray: bad.append(f"STRAY(n={stray})")
    for k,(tm,tw) in enumerate(keys):
        res,lat,nev=scored[k]
        if res=="OK":
            ok+=1; (lat_r if k in recon_keys else lat_h).append(lat)
        else: bad.append(f"k{k+1}:{res}(gap={gaps[k]:.1f}s,n={nev})")
    emit(f"{label} WORK seed={seed}: 60 keys over {act_s:.0f}s (gaps: {sum(1 for g in gaps[1:] if g<0.6)} short/{sum(1 for g in gaps[1:] if 1<=g<4)} med/{sum(1 for g in gaps[1:] if g>=5)} long), reconnect keys at this H={len(recon_keys)} | active mC={act_mC:.0f} to-lock mC={all_mC:.0f} over {all_s:.0f}s (cf {cf:.1f}) lock {'CENSORED' if censored else '%.0fs after last key'%(t_lock-t_wend)} | delivered {ok}/60 failures: {' '.join(bad) if bad else 'none'} | latency reconnect-keys {lat_summary(lat_r)} held-keys {lat_summary(lat_h)}")
if __name__=="__main__":
    p=argparse.ArgumentParser(); p.add_argument("label"); p.add_argument("hold_s",type=float); p.add_argument("--image",default="")
    p.add_argument("--charge",type=int,default=6); p.add_argument("--far",type=int,default=24); p.add_argument("--near",type=int,default=12)
    p.add_argument("--work",type=int,default=1); p.add_argument("--seed",type=int,default=7)
    a=p.parse_args(); H=a.hold_s
    img=hashlib.sha256(open(a.image,"rb").read()).hexdigest()[:16] if a.image and os.path.exists(a.image) else "n/a"
    emit(f"=== hold sweep v2 {a.label}: H={H}s image={img} charge={a.charge} far={a.far} near={a.near} work={a.work} | dongle={dongle()} ppk={stats_last(2)['mean_mA']:.3f}mA")
    wait_settled(); diag_snapshot("start")
    tp=Tapper(); rng=random.Random(a.seed+1)
    try:
        if a.charge: phase_charge(a.label,H,a.charge,tp)
        if a.far: phase_deliv(a.label,H,a.far,H+3.0,lambda r:r.uniform(-0.5,0.5),"far",tp,rng)
        if a.near: phase_deliv(a.label,H,a.near,max(0.0,H-2.0),lambda r:r.uniform(0.0,0.4),"near",tp,rng)
        if a.work: phase_work(a.label,H,a.seed,tp)
    finally: tp.close()
    wait_settled(); diag_snapshot("end")
    emit(f"=== {a.label} done  dongle={dongle()}")
