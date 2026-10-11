#!/usr/bin/env python3
# usage: tools/audit/judder.py build/audit/<route>/log.txt [from_step to_step]   (a run with WWHD_INTERP_CAM_TRACE=n
#   WWHD_INTERP_MODEL_TRACE=n WWHD_INTERP_MODEL_TRACE_FROM=<step> WWHD_INTERP_MODEL_TRACE_SET=1,11,42)
# per drawn frame: Link (the 42-joint model) and his boat (the model nearest under him at a window's first frame, then
# the same model: the King of Red Lions, the Big ship's hull), each relative to the drawn camera eye, and Link relative
# to the boat; how unevenly they move from frame to frame, per 30 s of logic steps (or the one step range given); and
# on how many half passes (t 0.5) the camera was drawn as its own 60 Hz step ("exact") instead of blended
import sys, re, math, collections
cam, link, models = {}, {}, collections.defaultdict(list)
for l in open(sys.argv[1], errors="replace"):
    m = re.search(r"\[interp\] camera \S+ step (\d+) t ([\d.]+) +(\S+) +eye ([-\d.]+) ([-\d.]+) ([-\d.]+)", l)
    if m: cam[(int(m[1]), m[2])] = (m[3], tuple(map(float, m.groups()[3:]))); continue
    m = re.search(r"\[interp\] model (\S+) \((\d+) joints, mtx \S+\) step (\d+) t ([\d.]+) +\S+ +root ([-\d.]+) ([-\d.]+) ([-\d.]+)", l)
    if m:
        k = (int(m[3]), m[4]); p = tuple(map(float, m.groups()[4:]))
        if m[2] == "42": link[k] = p
        else: models[k].append((m[1], p))
lo, hi = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (0, 1 << 62)
win = collections.defaultdict(list)
for k in sorted(k for k in link if k in cam and lo <= k[0] <= hi): win[0 if hi < 1 << 62 else k[0] // 900].append(k)
def rel(a, b): return tuple(a[i] - b[i] for i in range(3))
def uneven(seq):
    d = [rel(b, a) for a, b in zip(seq, seq[1:])]
    dd = sorted(math.dist(a, b) for a, b in zip(d, d[1:]))
    return (sum(dd) / len(dd), dd[int(len(dd) * .95)], dd[-1]) if dd else (float("nan"),) * 3
print("steps          frames  speed  cam60 | Link-camera: mean p95 max | boat-camera: mean p95 max | Link-boat: mean p95 max")
for w in sorted(win):
    ks = win[w]; L0 = link[ks[0]]
    under = [(math.dist(p, L0), a) for a, p in models[ks[0]] if p[1] <= L0[1] and math.dist(p, L0) < 1500]
    boat = min(under)[1] if under else None
    bp = {k: p for k in ks for a, p in models[k] if a == boat}
    lc = [rel(link[k], cam[k][1]) for k in ks]
    bk = [k for k in ks if k in bp]
    half = [k for k in ks if k[1] == "0.500"]
    sp = sum(math.dist(link[a], link[b]) for a, b in zip(ks, ks[1:])) / max(1, len(ks) - 1)
    a = uneven(lc); b = uneven([rel(bp[k], cam[k][1]) for k in bk]); c = uneven([rel(link[k], bp[k]) for k in bk])
    span = f"{ks[0][0]}-{ks[-1][0]}"
    print(f"{span:13s}  {len(ks):6d}  {sp:5.1f}  {sum(cam[k][0] == 'exact' for k in half):2d}/{len(half):<3d}| {a[0]:6.2f} {a[1]:6.2f} {a[2]:7.2f} | "
          f"{b[0]:6.2f} {b[1]:6.2f} {b[2]:7.2f} | {c[0]:6.2f} {c[1]:6.2f} {c[2]:7.2f}  (boat {boat})")
