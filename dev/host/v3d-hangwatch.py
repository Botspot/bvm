#!/usr/bin/env python3
"""Watch the V3D control-list registers for a stuck engine and log the hardware state during the hang.

Reads /sys/kernel/debug/dri/*/v3d_regs (readable without sudo here) every few ms.  The kernel's
timeout (v3d_cl_job_timedout) fires when the control-list current address (CTnCA) and return
address do not advance for ~0.5 s, so we do the same: when CT0 (binner) or CT1 (renderer) has
CA != EA (list not finished) and nothing changed for --stall seconds, print the full register
snapshot every 150 ms until progress resumes or the kernel resets the GPU.  A ring buffer of the
last --ring samples before the stall is written out too, so you see which addresses the engines
went through just before they stopped.

Times are CLOCK_MONOTONIC seconds (same clock as BVMJOB t_us/1e6 and gpu-trace*.txt).
Usage: ~/bvm/dev/host/v3d-hangwatch.py [--stall 0.25] [--interval 0.004] [--out FILE]
Stop with Ctrl+C.  Output default: ~/bvm/scratchpad/hangwatch-<stamp>.log
"""
import argparse, collections, glob, os, re, sys, time

ap = argparse.ArgumentParser()
ap.add_argument('--stall', type=float, default=0.25)
ap.add_argument('--interval', type=float, default=0.004)
ap.add_argument('--ring', type=int, default=300)
ap.add_argument('--out')
a = ap.parse_args()

path = None
for p in glob.glob('/sys/kernel/debug/dri/*/v3d_regs'):
    path = p
if not path:
    sys.exit('no v3d_regs in debugfs (need root? try sudo)')
out = a.out or os.path.expanduser('~/bvm/scratchpad/hangwatch-%s.log' % time.strftime('%Y%m%d-%H%M%S'))
f = open(out, 'w', buffering=1)
rx = re.compile(r'(V3D_\w+(?:\(\d+\))?) \(0x([0-9a-f]+)\): 0x([0-9a-f]+)')

def snap():
    with open(path) as fh:
        txt = fh.read()
    return {m.group(1): int(m.group(3), 16) for m in rx.finditer(txt)}

KEY = ['V3D_CLE_CT0CS', 'V3D_CLE_CT0CA', 'V3D_CLE_CT0EA',
       'V3D_CLE_CT1CS', 'V3D_CLE_CT1CA', 'V3D_CLE_CT1EA',
       'V3D_PTB_BPCA', 'V3D_PTB_BPCS']

def keyvals(s):
    return tuple(s.get(k, -1) for k in KEY)

def fmt(t, s):
    return '%.4f ' % t + ' '.join('%s=%08x' % (k.replace('V3D_', '').replace('CLE_', ''), s.get(k, 0)) for k in KEY)

def busy(s):
    return (s.get('V3D_CLE_CT0CA') != s.get('V3D_CLE_CT0EA')) or (s.get('V3D_CLE_CT1CA') != s.get('V3D_CLE_CT1EA'))

ring = collections.deque(maxlen=a.ring)
last = None
last_change = time.monotonic()
stalled = False
last_full = 0
nstalls = 0
f.write('# v3d-hangwatch %s stall=%.2fs\n' % (path, a.stall))
print('watching %s ; log: %s (Ctrl+C to stop)' % (path, out))
try:
    while True:
        t = time.monotonic()
        s = snap()
        kv = keyvals(s)
        if kv != last:
            ring.append((t, s))
            last = kv
            last_change = t
            if stalled:
                f.write('%.4f -- progress resumed after %.3fs --\n' % (t, t - stall_start))
                stalled = False
        elif busy(s) and t - last_change >= a.stall:
            if not stalled:
                stalled = True
                stall_start = last_change
                nstalls += 1
                f.write('\n==== STALL #%d: no CT/PTB change since %.4f (now %.4f) ====\n' % (nstalls, last_change, t))
                f.write('-- last %d register changes before the stall --\n' % len(ring))
                for rt, rs in ring:
                    f.write(fmt(rt, rs) + '\n')
                f.write('-- full snapshot at detection --\n')
                for k, v in sorted(s.items()):
                    f.write('  %s = 0x%08x\n' % (k, v))
                last_full = t
                print('STALL #%d detected at %.3f (CT0 %08x/%08x CT1 %08x/%08x)' % (
                    nstalls, t, s['V3D_CLE_CT0CA'], s['V3D_CLE_CT0EA'], s['V3D_CLE_CT1CA'], s['V3D_CLE_CT1EA']))
            elif t - last_full >= 0.15:
                f.write('%.4f still stalled: ' % t + fmt(t, s)[len('%.4f ' % t):] +
                        ' ERR=%08x FDBGO=%08x FDBGB=%08x FDBGS=%08x INT=%08x\n' % (
                            s.get('V3D_ERR_STAT', 0), s.get('V3D_ERR_FDBGO', 0), s.get('V3D_ERR_FDBGB', 0),
                            s.get('V3D_ERR_FDBGS', 0), s.get('V3D_CTL_INT_STS', 0)))
                last_full = t
        time.sleep(a.interval)
except KeyboardInterrupt:
    pass
print('%d stall(s) logged to %s' % (nstalls, out))
