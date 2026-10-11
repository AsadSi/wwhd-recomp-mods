#!/usr/bin/env python3
# usage: tools/audit/judder.py build/audit/<route>/log.txt   (a run with WWHD_INTERP_CAM_TRACE=n WWHD_INTERP_MODEL_TRACE=n
#   WWHD_INTERP_MODEL_TRACE_FROM=<step> WWHD_INTERP_MODEL_TRACE_SET=11,42)
# per drawn frame: Link (the 42-joint model) and the King of Red Lions (the 11-joint model nearest under him), each
# relative to the drawn camera eye; how unevenly they move from frame to frame, per 30 s of logic steps
import sys, re, math, collections
cam, link, lion = {}, {}, collections.defaultdict(list)
for l in open(sys.argv[1], errors="replace"):
    m = re.search(r"\[interp\] camera \S+ step (\d+) t ([\d.]+) +\S+ +eye ([-\d.]+) ([-\d.]+) ([-\d.]+)", l)
    if m: cam[(int(m[1]), m[2])] = tuple(map(float, m.groups()[2:])); continue
    m = re.search(r"\[interp\] model (\S+) \((\d+) joints, mtx \S+\) step (\d+) t ([\d.]+) +\S+ +root ([-\d.]+) ([-\d.]+) ([-\d.]+)", l)
    if m:
        k = (int(m[3]), m[4]); p = tuple(map(float, m.groups()[4:]))
        if m[2] == "42": link[k] = p
        else: lion[k].append(p)
keys = sorted(k for k in link if k in cam)
rows = []
for k in keys:
    L = link[k]; c = cam[k]
    near = min(lion.get(k, []), key=lambda p: math.dist(p, L), default=None)
    if near is not None and math.dist(near, L) > 400: near = None
    rows.append((k, tuple(L[i] - c[i] for i in range(3)), None if near is None else tuple(near[i] - c[i] for i in range(3)), L))
def uneven(seq):
    d = [tuple(b[i] - a[i] for i in range(3)) for a, b in zip(seq, seq[1:])]
    dd = sorted(math.dist(a, b) for a, b in zip(d, d[1:]))
    return (sum(dd) / len(dd), dd[int(len(dd) * .95)], dd[-1]) if dd else (0, 0, 0)
win = collections.defaultdict(list)
for r in rows: win[r[0][0] // 900].append(r)
print("30 s window (steps)  frames  speed | Link from camera: mean p95 max | Lion from camera: mean p95 max")
for w in sorted(win):
    rs = win[w]
    sp = sum(math.dist(a[3], b[3]) for a, b in zip(rs, rs[1:])) / max(1, len(rs) - 1)
    a = uneven([r[1] for r in rs])
    lv = [r[2] for r in rs if r[2] is not None]
    b = uneven(lv) if len(lv) > 3 else (float("nan"),) * 3
    print(f"{w*900:6d}-{w*900+899:<6d}  {len(rs):6d}  {sp:5.1f} | {a[0]:6.2f} {a[1]:6.2f} {a[2]:7.2f} | {b[0]:6.2f} {b[1]:6.2f} {b[2]:7.2f}  ({len(lv)} Lion)")
