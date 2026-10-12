"""Twilight Princess HD's Shadow Beast (res/Object/E_s2.arc + E_s2.pack.gz, the archive daE_S1 loads its model and
animations from) -> game/shadow_beast.bin, the skinned mesh, textures and animations the Shadow stalker mod draws
(runtime/src/gfx/beast.mm).

    tools/shadow_beast.py [<TP HD game dir>] [<out>]   (defaults ~/Documents/code/tphd-recomp/game, game/shadow_beast.bin)

The mesh is the J3D model's own (s2.bmd: VTX1/SHP1 skinned through DRW1/EVP1; the HD pack's .gmx has the same
triangles), in bind pose model space; the textures are the HD pack's GX2 ones (the body 1024x1024), untiled and the
archive unpacked with the ingame-mod tools' libgx2tex and libyaz0 (GX2TEX_DIR overrides where);
each .bck is sampled at every frame into joint-local scale, quaternion, translation.

Output, little-endian: 'SBST' u32 version, then
  joints:  u32 n, n x (i32 parent, f32 inverse bind 3x4 row-major)
  verts:   u32 n, n x (f32 pos3, nrm3, uv2, u8 joint4, f32 weight4)
  indices: u32 n, n x u16 (triangles)
  groups:  u32 n, n x (u32 texture, first index, index count, flags)   flags: 1 = the mark (s2_mat_mark: unlit, its
           colour TEV register C2 times the texture, cut out below alpha 128); bits 8+: GX cull mode
  textures: u32 n, n x (u32 w, h, w*h*4 RGBA8)
  anims:   u32 n, n x (char[32] name, u32 frames, u32 loop, frames x joints x f32[10] (scale3, quat xyzw, trans3))
"""
import math, os, struct, sys, gzip

TP = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Documents/code/tphd-recomp/game")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "game", "shadow_beast.bin")
sys.path.insert(0, os.environ.get("GX2TEX_DIR", os.path.expanduser("~/Documents/Higgsfield export/ingame-mod/tools")))
import arc, gx2tex  # noqa: E402
import numpy as np  # noqa: E402


def u8(d, o): return d[o]
def u16(d, o): return struct.unpack_from(">H", d, o)[0]
def s16(d, o): return struct.unpack_from(">h", d, o)[0]
def u32(d, o): return struct.unpack_from(">I", d, o)[0]
def f32(d, o): return struct.unpack_from(">f", d, o)[0]


def rarc(d):
    """RARC -> {path: bytes} (files only)."""
    assert d[:4] == b"RARC"
    data_off = u32(d, 0xC) + 0x20
    info = 0x20
    nnodes, node_off, nent, ent_off, _, str_off = (u32(d, info), u32(d, info + 4) + 0x20, u32(d, info + 8),
                                                     u32(d, info + 0xC) + 0x20, u32(d, info + 0x10), u32(d, info + 0x14) + 0x20)
    def name(o): return d[str_off + o:d.index(b"\0", str_off + o)].decode()
    out = {}
    def walk(n, path):
        no = node_off + 0x10 * n
        cnt, first = u16(d, no + 0xA), u32(d, no + 0xC)
        for i in range(first, first + cnt):
            e = ent_off + 0x14 * i
            typ, nm = u8(d, e + 4), name(u16(d, e + 6))
            if nm in (".", ".."): continue
            if typ & 2: walk(u32(d, e + 8), path + nm + "/")
            else: out[path + nm] = d[data_off + u32(d, e + 8):data_off + u32(d, e + 8) + u32(d, e + 0xC)]
    walk(0, "")
    return out


def sections(d):
    out, o = {}, 0x20
    for _ in range(u32(d, 0xC)):
        out[d[o:o + 4].decode()] = o
        o += u32(d, o + 4)
    return out


def names(d, o):
    return [d[o + u16(d, o + 6 + 4 * i):d.index(b"\0", o + u16(d, o + 6 + 4 * i))].decode() for i in range(u16(d, o))]


# ---------------------------------------------------------------- math (3x4 row-major lists)
def mat(S, R, T):
    return [[R[i][j] * S[j] for j in range(3)] + [T[i]] for i in range(3)]


def euler(x, y, z):  # J3D: R = Rz * Ry * Rx
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    return [[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
            [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
            [-sy, sx * cy, cx * cy]]


def mul(A, B):
    return [[sum(A[i][k] * B[k][j] for k in range(3)) + (A[i][3] if j == 3 else 0) for j in range(4)] for i in range(3)]


def inv(M):
    a = [r[:3] for r in M]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    c = [[(a[(j + 1) % 3][(i + 1) % 3] * a[(j + 2) % 3][(i + 2) % 3] - a[(j + 1) % 3][(i + 2) % 3] * a[(j + 2) % 3][(i + 1) % 3]) / det
          for j in range(3)] for i in range(3)]
    t = [-sum(c[i][k] * M[k][3] for k in range(3)) for i in range(3)]
    return [c[i] + [t[i]] for i in range(3)]


def xf(M, v, w=1.0):
    return [M[i][0] * v[0] + M[i][1] * v[1] + M[i][2] * v[2] + M[i][3] * w for i in range(3)]


def quat(R):  # rotation matrix (columns may be scaled) -> x, y, z, w
    m = [[R[i][j] for j in range(3)] for i in range(3)]
    for j in range(3):
        n = math.sqrt(sum(m[i][j] ** 2 for i in range(3))) or 1
        for i in range(3): m[i][j] /= n
    t = m[0][0] + m[1][1] + m[2][2]
    if t > 0:
        s = math.sqrt(t + 1) * 2
        return [(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, s / 4]
    i = max(range(3), key=lambda k: m[k][k])
    j, k = (i + 1) % 3, (i + 2) % 3
    s = math.sqrt(1 + m[i][i] - m[j][j] - m[k][k]) * 2
    q = [0, 0, 0, 0]
    q[i], q[j], q[k], q[3] = s / 4, (m[j][i] + m[i][j]) / s, (m[k][i] + m[i][k]) / s, (m[k][j] - m[j][k]) / s
    return q


# ---------------------------------------------------------------- BMD
def load_bmd(d):
    sec = sections(d)
    # JNT1: bind pose local transforms
    j = sec["JNT1"]
    nj, ent, remap = u16(d, j + 8), j + u32(d, j + 0xC), j + u32(d, j + 0x10)
    jnames = names(d, j + u32(d, j + 0x14))
    local = []
    for i in range(nj):
        e = ent + 0x40 * u16(d, remap + 2 * i)
        S = [f32(d, e + 4 + 4 * k) for k in range(3)]
        R = [s16(d, e + 0x10 + 2 * k) * math.pi / 32768 for k in range(3)]
        T = [f32(d, e + 0x18 + 4 * k) for k in range(3)]
        local.append((S, R, T))
    # INF1: joint parents and each shape's material
    o = sec["INF1"] + u32(d, sec["INF1"] + 0x14)
    parent, shape_mat = [-1] * nj, {}
    stack, last, cur_mat, mat_stack = [], None, None, []
    while True:
        typ, idx = u16(d, o), u16(d, o + 2)
        o += 4
        if typ == 0: break
        if typ == 1: stack.append(last); mat_stack.append(cur_mat)
        elif typ == 2: stack.pop(); cur_mat = mat_stack.pop()
        elif typ == 0x10:
            parent[idx] = next((n[1] for n in reversed(stack) if n and n[0] == "j"), -1)
            last = ("j", idx)
        elif typ == 0x11: cur_mat = idx; last = ("m", idx)
        elif typ == 0x12: shape_mat[idx] = cur_mat; last = ("s", idx)
    world = []
    for i in range(nj):
        S, R, T = local[i]
        m = mat(S, euler(*R), T)
        world.append(mul(world[parent[i]], m) if parent[i] >= 0 else m)
    # EVP1 envelopes, DRW1 draw matrices
    e = sec["EVP1"]
    nenv = u16(d, e + 8)
    cnt_o, idx_o, w_o, ib_o = (e + u32(d, e + 0xC + 4 * k) for k in range(4))
    envs, p, ib = [], 0, {}
    for i in range(nenv):
        n = u8(d, cnt_o + i)
        envs.append([(u16(d, idx_o + 2 * (p + k)), f32(d, w_o + 4 * (p + k))) for k in range(n)])
        p += n
    inv_bind = []
    for i in range(nj):
        if ib_o > e and ib_o + 0x30 * (i + 1) <= sec["DRW1"]:
            inv_bind.append([[f32(d, ib_o + 0x30 * i + 4 * (4 * r + c)) for c in range(4)] for r in range(3)])
        else:
            inv_bind.append(inv(world[i]))
    w = sec["DRW1"]
    ndrw = u16(d, w + 8)
    drw = [(u8(d, w + u32(d, w + 0xC) + i), u16(d, w + u32(d, w + 0x10) + 2 * i)) for i in range(ndrw)]
    # VTX1 arrays
    v = sec["VTX1"]
    fmt_o = v + u32(d, v + 8)
    offs = [u32(d, v + 0xC + 4 * k) for k in range(13)]
    fmts = {}
    o = fmt_o
    while u32(d, o) != 0xFF:
        fmts[u32(d, o)] = (u32(d, o + 4), u32(d, o + 8), u8(d, o + 0xC))
        o += 0x10
    def array(attr, slot, ncomp):
        cc, ct, frac = fmts[attr]
        start = v + offs[slot]
        later = [x for x in offs if x > offs[slot]]
        end = v + (min(later) if later else u32(d, v + 4))
        size, code = {0: (1, "B"), 1: (1, "b"), 2: (2, "H"), 3: (2, "h"), 4: (4, "f")}[ct]
        scale = 1.0 if ct == 4 else 1.0 / (1 << frac)
        n = (end - start) // (size * ncomp)
        return [[x * scale for x in struct.unpack_from(">%d%s" % (ncomp, code), d, start + i * size * ncomp)] for i in range(n)]
    pos = array(9, 0, 3)
    nrm = array(10, 1, 3) if 10 in fmts else None
    uv = array(13, 5, 2) if 13 in fmts else None
    # SHP1 shapes -> triangles of (pos, nrm, uv, skin) corners
    s = sec["SHP1"]
    nshp = u16(d, s + 8)
    shp_o, remap_o, attr_o, mtxt_o, prim_o, mtxd_o, pkt_o = (s + u32(d, s + k) for k in (0xC, 0x10, 0x18, 0x1C, 0x20, 0x24, 0x28))
    shapes = []
    for si in range(nshp):
        e = shp_o + 0x28 * u16(d, remap_o + 2 * si)
        npkt, aoff, first_mtx, first_pkt = u16(d, e + 2), u16(d, e + 4), u16(d, e + 6), u16(d, e + 8)
        attrs, o = [], attr_o + aoff
        while u32(d, o) != 0xFF:
            attrs.append((u32(d, o), u32(d, o + 4)))
            o += 8
        tris, slots = [], [0xFFFF] * 10
        for pk in range(npkt):
            md = mtxd_o + 8 * (first_mtx + pk)
            mcount, mfirst = u16(d, md + 2), u32(d, md + 4)
            for k in range(mcount):
                x = u16(d, mtxt_o + 2 * (mfirst + k))
                if x != 0xFFFF: slots[k] = x
            pl = pkt_o + 8 * (first_pkt + pk)
            o, end = prim_o + u32(d, pl + 4), prim_o + u32(d, pl + 4) + u32(d, pl)
            while o < end:
                op = u8(d, o)
                if op == 0: break
                nv = u16(d, o + 1)
                o += 3
                verts = []
                for _ in range(nv):
                    vals = {}
                    for a, t in attrs:
                        if t == 1: vals[a] = u8(d, o); o += 1
                        elif t == 2: vals[a] = u8(d, o); o += 1
                        elif t == 3: vals[a] = u16(d, o); o += 2
                    verts.append((dict(vals), list(slots)))
                kind = op & 0xF8
                if kind == 0x90: idx = [(i, i + 1, i + 2) for i in range(0, nv - 2, 3)]
                elif kind == 0x98: idx = [(i, i + 1, i + 2) if i % 2 == 0 else (i + 1, i, i + 2) for i in range(nv - 2)]
                elif kind == 0xA0: idx = [(0, i + 1, i + 2) for i in range(nv - 2)]
                else: raise SystemExit("primitive %#x" % op)
                tris += [tuple(verts[k] for k in t) for t in idx]
        shapes.append((shape_mat.get(si), tris))
    # MAT3: each material's first texture and cull mode
    m = sec["MAT3"]
    init_o, mremap_o, texno_o, cull_o = m + u32(d, m + 0xC), m + u32(d, m + 0x10), m + u32(d, m + 0x48), m + u32(d, m + 0x1C)
    mnames = names(d, m + u32(d, m + 0x14))
    def material(i):
        e = init_o + 0x14C * u16(d, mremap_o + 2 * i)
        t = u16(d, e + 0x84)
        return mnames[i], (u16(d, texno_o + 2 * t) if t != 0xFFFF else None), u32(d, cull_o + 4 * u8(d, e + 1))
    t = sec["TEX1"]
    tnames = names(d, t + u32(d, t + 0x10))

    def skin(vals, slots):
        mi = slots[vals.get(0, 0) // 3]
        weighted, ix = drw[mi]
        if weighted: return envs[ix]
        return [(ix, 1.0)]
    corners = {}
    verts, indices, groups = [], [], []
    for mi, tris in shapes:
        mname, tex, cull = material(mi)
        first = len(indices)
        for tri in tris:
            for vals, slots in tri:
                sk = skin(vals, slots)
                p = pos[vals[9]]
                n = nrm[vals[10]] if nrm and 10 in vals else [0, 1, 0]
                if len(sk) == 1 and not drw[slots[vals.get(0, 0) // 3]][0]:  # rigid: positions are joint-local
                    p, n = xf(world[sk[0][0]], p), xf(world[sk[0][0]], n, 0)
                ln = math.sqrt(sum(c * c for c in n)) or 1
                key = (tuple(round(c, 4) for c in p), tuple(round(c / ln, 3) for c in n),
                       tuple(uv[vals[13]]) if uv and 13 in vals else (0, 0), tuple(sk))
                if key not in corners:
                    corners[key] = len(verts)
                    verts.append(key)
                indices.append(corners[key])
        groups.append((tex, first, len(indices) - first, mname, cull))
        print("shape: material %-16s texture %-16s cull %d, %d triangles" % (mname, tnames[tex] if tex is not None else "-", cull, len(tris)))
    # weld the inverse binds to the vertices: envelope vertices are in the EVP1 bind space, rigid ones were put in
    # model space with JNT1's world, so a joint without an EVP1 matrix uses JNT1's inverse
    return jnames, parent, inv_bind, verts, indices, groups, tnames, world


# ---------------------------------------------------------------- BCK
def hermite(t, k0, k1):
    t0, v0, _, o0 = k0
    t1, v1, i1, _ = k1
    dt = t1 - t0
    if dt <= 0: return v0
    s = (t - t0) / dt
    return (v0 * (2 * s ** 3 - 3 * s ** 2 + 1) + v1 * (-2 * s ** 3 + 3 * s ** 2)
            + (o0 * (s ** 3 - 2 * s ** 2 + s) + i1 * (s ** 3 - s ** 2)) * dt)


def load_bck(d):
    a = sections(d)["ANK1"]
    loop, angmul, length, nj = u8(d, a + 8), u8(d, a + 9), u16(d, a + 0xA), u16(d, a + 0xC)
    joint_o, sc_o, rot_o, tr_o = (a + u32(d, a + 0x14 + 4 * k) for k in range(4))
    rscale = (1 << angmul) * math.pi / 32768
    def track(o, arr, kind):
        count, index, tan = u16(d, o), u16(d, o + 2), u16(d, o + 4)
        if kind == "r": val = lambda i: s16(d, arr + 2 * i) * 1.0
        else: val = lambda i: f32(d, arr + 4 * i)
        sc = rscale if kind == "r" else 1.0
        if count == 1: return lambda t: val(index) * sc
        step = 4 if tan == 1 else 3
        keys = []
        for k in range(count):
            b = index + k * step
            tt, vv, ti = val(b), val(b + 1), val(b + 2)
            to = val(b + 3) if tan == 1 else ti
            if kind == "r": tt = tt  # times are plain numbers in the s16 array too
            keys.append((tt, vv * sc, ti * sc, to * sc))
        def f(t):
            if t <= keys[0][0]: return keys[0][1]
            for k0, k1 in zip(keys, keys[1:]):
                if t <= k1[0]: return hermite(t, k0, k1)
            return keys[-1][1]
        return f
    joints = []
    for j in range(nj):
        o = joint_o + 0x36 * j
        tr = {}
        for ax in range(3):
            base = o + ax * 0x12
            tr[("s", ax)] = track(base, sc_o, "s")
            tr[("r", ax)] = track(base + 6, rot_o, "r")
            tr[("t", ax)] = track(base + 0xC, tr_o, "t")
        joints.append(tr)
    frames = []
    for fr in range(max(length, 1)):
        row = []
        for tr in joints:
            S = [tr[("s", k)](fr) for k in range(3)]
            R = [tr[("r", k)](fr) for k in range(3)]
            T = [tr[("t", k)](fr) for k in range(3)]
            row.append((S, quat(euler(*R)), T))
        frames.append(row)
    return loop, frames


def bc1(data, w, h):
    """BC1 blocks (linear) -> HxWx4 RGBA8."""
    a = np.frombuffer(data[:w * h // 2], np.uint8).reshape(-1, 8).astype(np.uint32)
    c0, c1 = a[:, 0] | a[:, 1] << 8, a[:, 2] | a[:, 3] << 8
    idx = a[:, 4] | a[:, 5] << 8 | a[:, 6] << 16 | a[:, 7] << 24
    def rgb(v): return np.stack([(v >> 11 & 31) * 255 / 31, (v >> 5 & 63) * 255 / 63, (v & 31) * 255 / 31, np.full(v.shape, 255.)], -1)
    p0, p1, gt = rgb(c0), rgb(c1), (c0 > c1)[:, None]
    p3 = np.where(gt, (p0 + 2 * p1) / 3, 0)
    pal = np.stack([p0, p1, np.where(gt, (2 * p0 + p1) / 3, (p0 + p1) / 2), p3], 1)
    sel = np.stack([idx >> (2 * k) & 3 for k in range(16)], 1)
    px = np.take_along_axis(pal, sel[:, :, None].repeat(4, 2).astype(np.int64), 1)
    return px.reshape(h // 4, w // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(h, w, 4).round().astype(np.uint8)


def gtx_textures(gtx):
    """A GTX's textures (base level) as (w, h, RGBA8 bytes)."""
    o, cur, out = u32(gtx, 4), None, []
    while o < len(gtx):
        hs, t, ds = u32(gtx, o + 4), u32(gtx, o + 0x10), u32(gtx, o + 0x14)
        b = gtx[o + hs:o + hs + ds]
        if t == 0xB: cur = {"s": bytearray(b[:0x74])}; out.append(cur)
        elif t == 0xC: cur["img"] = b
        elif t == 0xD: cur["mip"] = b
        o += hs + ds
    res = []
    for tx in out:
        f = gx2tex.fields(tx["s"])
        w, h = f["w"], f["h"]
        lin = gx2tex.untile(tx["s"], tx["img"], tx.get("mip", b""))
        if f["fmt"] == 0x31: px = bc1(lin, w, h)
        elif f["fmt"] == 0x1: px = np.repeat(np.frombuffer(lin[:w * h], np.uint8).reshape(h, w, 1), 4, 2)  # I8
        else: raise SystemExit("texture format %#x" % f["fmt"])
        res.append((w, h, px.tobytes()))
    return res


def pack_files(pack):
    """A TP HD .pack.gz (TMPK) -> {path: bytes}."""
    d = gzip.decompress(pack)
    assert d[:4] == b"TMPK"
    out = {}
    for i in range(u32(d, 4)):
        no, do, sz = struct.unpack_from(">III", d, 0x10 + 16 * i)
        out[d[no:d.index(b"\0", no)].decode()] = d[do:do + sz]
    return out


def main():
    obj = os.path.join(TP, "content/res/Object/")
    arc_files = rarc(arc.yaz0(open(obj + "E_s2.arc", "rb").read()))
    jnames, parent, inv_bind, verts, indices, groups, tnames, world = load_bmd(arc_files["bmdr/s2.bmd"])
    texs = gtx_textures(pack_files(open(obj + "E_s2.pack.gz", "rb").read())["tex/Object/E_s2/archive/bmdr/s2.bmd.gtx"])
    anims = []
    for path in sorted(arc_files):
        if path.endswith(".bck"):
            loop, frames = load_bck(arc_files[path])
            assert len(frames[0]) == len(jnames), (path, len(frames[0]))
            anims.append((os.path.basename(path)[:-4], loop, frames))
    out = bytearray(b"SBST" + struct.pack("<I", 1))
    out += struct.pack("<I", len(jnames))
    for p, m in zip(parent, inv_bind):
        out += struct.pack("<i12f", p, *[x for r in m for x in r])
    out += struct.pack("<I", len(verts))
    for p, n, t, sk in verts:
        sk = sorted(sk, key=lambda x: -x[1])[:4]
        tot = sum(w for _, w in sk) or 1
        js = [j for j, _ in sk] + [0] * (4 - len(sk))
        ws = [w / tot for _, w in sk] + [0] * (4 - len(sk))
        out += struct.pack("<8f4B4f", *p, *n, *t, *js, *ws)
    out += struct.pack("<I", len(indices)) + struct.pack("<%dH" % len(indices), *indices)
    out += struct.pack("<I", len(groups))
    for tex, first, count, mname, cull in groups:
        flags = 1 if mname == "s2_mat_mark" else 0
        out += struct.pack("<4I", tex if tex is not None else 0xFFFFFFFF, first, count, flags | (cull << 8))
    out += struct.pack("<I", len(texs))
    for w, h, px in texs: out += struct.pack("<2I", w, h) + px
    out += struct.pack("<I", len(anims))
    for name, loop, frames in anims:
        out += struct.pack("<32s2I", name.encode(), len(frames), loop)
        for row in frames:
            for S, Q, T in row: out += struct.pack("<10f", *S, *Q, *T)
    os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
    open(OUT, "wb").write(out)
    lo = [min(v[0][i] for v in verts) for i in range(3)]
    hi = [max(v[0][i] for v in verts) for i in range(3)]
    print("%d joints, %d vertices, %d triangles, %d textures (%s), %d animations -> %s (%d bytes)" % (
        len(jnames), len(verts), len(indices) // 3, len(texs), ", ".join(tnames), len(anims), OUT, len(out)))
    print("bind pose bounds", [round(x) for x in lo], [round(x) for x in hi])
    print("joints", " ".join("%d:%s<%d" % (i, n, p) for i, (n, p) in enumerate(zip(jnames, parent))))
    print("animations", " ".join("%s:%d%s" % (n, len(f), "L" if l else "") for n, l, f in anims))


if __name__ == "__main__":
    main()
