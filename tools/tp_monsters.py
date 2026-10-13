"""Twilight Princess HD's monsters for Wind Waker HD: their J3D models (with their armour and weapons), HD textures and
animations -> game/tp_<monster>.bin, which runtime/src/mods/tp_monsters.cpp poses and gfx/beast.mm draws.

    tools/tp_monsters.py [<TP HD game dir>]   (default ~/Documents/code/tphd-recomp/game; writes game/tp_*.bin)

  shadow_beast  daE_S1's field Shadow Beast: E_s2.arc s2.bmd and its 27 .bck (the archive d_a_e_s1.cpp loads)
  darknut       daB_TN's Darknut, its "zako" type 1 (Hyrule Castle's): B_tnp2.arc tn2.bmd, its 17 armour and weapon
                models on their joints (tn_part_joint), their .brk colour variants (frames 0-4), B_tn.arc's 42 .bck

Meshes are the J3D models' own (VTX1/SHP1 skinned through DRW1/EVP1; the HD packs' .gmx have the same triangles), in
bind pose model space; a part's mesh is put on its joint and skinned to it alone (the runtime can move it anywhere:
a broken-off piece). Textures are the HD packs' GX2 ones, untiled and the archives unpacked with the ingame-mod tools'
libgx2tex and libyaz0 (GX2TEX_DIR overrides where); each .bck is sampled at every frame into joint-local scale,
quaternion, translation.

Output, little-endian: 'SBST' u32 version 2, then
  joints:  u32 n, n x (i32 parent, f32 inverse bind 3x4 row-major)
  verts:   u32 n, n x (f32 pos3, nrm3, uv2, u8 joint4, f32 weight4)
  indices: u32 n, n x u16 (triangles)
  groups:  u32 n, n x (u32 texture, first index, index count, flags, part, f32 light scale, u32 c0[5], c2[5])
           flags: bits 0-1 the TEV set-up (0 lit, 1 unlit C2 x texture, 2 C2-to-white x lit), bit 2 cut out below
           alpha 128, bits 8+ GX cull mode; part 0 the body, k part k-1; c0/c2 RGBA8 per colour variant
  parts:   u32 n, n x (char[32] name, i32 joint)
  textures: u32 n, n x (u32 w, h, w*h*4 RGBA8)
  anims:   u32 n, n x (char[32] name, u32 frames, u32 loop, frames x joints x f32[10] (scale3, quat xyzw, trans3))
"""
import math, os, struct, sys, gzip

TP = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Documents/code/tphd-recomp/game")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "game")
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
    # MAT3: each material's first texture, cull mode, and which of TP's TEV set-ups it is (kind): 0 lit (texture x the lit
    # vertex colour, the stage's scale), 1 the mark (unlit: TEV colour C2 x the texture), 2 armour (stage 0: C2 to white
    # by the texture, stage 1: that x the lit colour, its scale); bit 2: cut out below alpha 128 (alpha compare GEQUAL)
    m = sec["MAT3"]
    tab = [m + u32(d, m + 0xC + 4 * k) for k in range(30)]
    init_o, mremap_o, texno_o, cull_o = tab[0], tab[1], tab[15], tab[4]
    mnames = names(d, tab[2])
    def material(i):
        e = init_o + 0x14C * u16(d, mremap_o + 2 * i)
        t = u16(d, e + 0x84)
        stages = u8(d, tab[19] + u8(d, e + 4))
        st = [tab[20] + 20 * u16(d, e + 0xE4 + 2 * k) for k in range(stages)]
        cin = lambda s: tuple(u8(d, s + 1 + k) for k in range(4))
        scale = lambda s: {0: 1.0, 1: 2.0, 2: 4.0, 3: 0.5}[u8(d, s + 7)]
        if cin(st[0]) == (15, 6, 8, 15): kind, mult = 1, 1.0
        elif stages >= 2 and cin(st[0]) == (6, 14, 8, 15): kind, mult = 2, scale(st[1])
        else: kind, mult = 0, scale(st[0])
        ac = tab[24] + 8 * u16(d, e + 0x146)
        if u8(d, ac) == 6 and u8(d, ac + 1) > 0: kind |= 4
        c2i = u16(d, e + 0xDC + 4)
        c2 = tuple(max(0, min(255, s16(d, tab[17] + 8 * c2i + 2 * k))) for k in range(3)) if c2i != 0xFFFF else (0, 0, 0)
        return (mnames[i], (u16(d, texno_o + 2 * t) if t != 0xFFFF else None), u32(d, cull_o + 4 * u8(d, e + 1)), kind,
                mult / 2, c2)  # (lighting is WW's, half TP's: TP's dim scene lights x its 2 came out as WW's light x 1)
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
        mname, tex, cull, kind, mult, c2 = material(mi)
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
        groups.append((tex, first, len(indices) - first, mname, cull, kind, mult, c2))
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
        elif f["fmt"] == 0x7:  # R8G8 as IA8: intensity, alpha
            ia = np.frombuffer(lin[:w * h * 2], np.uint8).reshape(h, w, 2)
            px = np.dstack([ia[..., 0], ia[..., 0], ia[..., 0], ia[..., 1]])
        elif f["fmt"] == 0x2:  # R4G4 as IA4
            b = np.frombuffer(lin[:w * h], np.uint8).reshape(h, w)
            i, a = (b & 15) * 17, (b >> 4) * 17
            px = np.dstack([i, i, i, a]).astype(np.uint8)
        elif f["fmt"] == 0x1A:  # RGBA8
            px = np.frombuffer(lin[:w * h * 4], np.uint8).reshape(h, w, 4)
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


# ---------------------------------------------------------------- BRK (TEV register colour animations)
def load_brk(d):
    """{material name: {register: [(r, g, b, a) at frames 0..4]}} for the C0-C2 registers."""
    t = sections(d)["TRK1"]
    nreg = u16(d, t + 0xC)
    tab, remap, nm = (t + u32(d, t + 0x20 + 4 * k) for k in (0, 2, 4))
    data = [t + u32(d, t + 0x38 + 4 * k) for k in range(4)]  # register R, G, B, A key arrays (s16)
    mats = names(d, nm)
    def channel(o, arr, frame):
        count, index, tan = u16(d, o), u16(d, o + 2), u16(d, o + 4)
        val = lambda i: s16(d, arr + 2 * i)
        if count == 1: return val(index)
        step = 4 if tan == 1 else 3
        keys = [(val(index + k * step), val(index + k * step + 1), val(index + k * step + 2),
                 val(index + k * step + (3 if tan == 1 else 2))) for k in range(count)]
        if frame <= keys[0][0]: return keys[0][1]
        for k0, k1 in zip(keys, keys[1:]):
            if frame <= k1[0]: return hermite(frame, k0, k1)
        return keys[-1][1]
    out = {}
    for i in range(nreg):
        e = tab + 0x1C * u16(d, remap + 2 * i)
        reg = u8(d, e + 0x18)
        out.setdefault(mats[i], {})[reg] = [tuple(max(0, min(255, int(round(channel(e + 6 * c, data[c], f))))) for c in range(4))
                                            for f in range(5)]
    return out


def rgba(c): return c[0] | c[1] << 8 | c[2] << 16 | (c[3] if len(c) > 3 else 255) << 24


SPECS = {
    "shadow_beast": dict(arc="E_s2", body="s2", anims="E_s2", parts=[]),
    # tn_part_model_id (type 1) and tn_part_joint; the mace's slot holds sword_a when its mType2 is 1, so both are kept
    # (sword_a last); tn_model_brk_idx: 0 color_1, 1 color_2, 2 shield, -1 none
    "darknut": dict(arc="B_tnp2", body="tn2", anims="B_tn", body_brk="tn2_body", parts=[
        ("tn2_armor_arm_l", 8, 0), ("tn2_armor_arm_r", 14, 0), ("tn2_armor_chest_b", 3, 0), ("tn2_armor_chest_f", 3, 0),
        ("tn2_armor_head_a", 5, 0), ("tn2_armor_head_b", 5, 1), ("tn2_armor_shoulder_l", 11, 1), ("tn2_armor_shoulder_r", 17, 1),
        ("tn2_armor_waist_b", 26, 0), ("tn2_armor_waist_f", 25, 0), ("tn2_armor_waist_l", 27, 1), ("tn2_armor_waist_r", 28, 1),
        ("tn2_shield", 9, 2), ("tn2_mace", 27, -1), ("tn2_sword_b_saya", 27, -1), ("tn2_sword_b", 27, -1), ("tn2_sword_a", 27, -1)]),
}
kBrk = ["tn2_color_1", "tn2_color_2", "tn2_shield"]


def build(name, spec):
    obj = os.path.join(TP, "content/res/Object/")
    def archive(n):
        d = open(obj + n + ".arc", "rb").read()
        return rarc(arc.yaz0(d))
    files = archive(spec["arc"])
    pack = pack_files(open(obj + spec["arc"] + ".pack.gz", "rb").read())
    sub = "bmdr" if any(k.startswith("bmdr/") for k in files) else "bmdp"
    def model(n):
        res = load_bmd(files["%s/%s.bmd" % (sub, n)])
        return res, gtx_textures(pack["tex/Object/%s/archive/%s/%s.bmd.gtx" % (spec["arc"], sub, n)])
    (jnames, parent, inv_bind, verts, indices, groups, tnames, world), texs = model(spec["body"])
    brk = lambda n: load_brk(files["brk/%s.brk" % n]) if "brk/%s.brk" % n in files else {}
    body_brk = brk(spec.get("body_brk", ""))
    textures, by_bytes = [], {}
    def texture(t):  # de-duplicated (the armour pieces all carry the same armour texture)
        key = (t[0], t[1], hash(t[2]))
        if key not in by_bytes:
            by_bytes[key] = len(textures)
            textures.append(t)
        return by_bytes[key]
    out_groups = []
    def add_groups(gs, tex_list, part, brk_regs, first_index, first_vert):
        for tex, first, count, mname, cull, kind, mult, c2 in gs:
            regs = brk_regs.get(mname, {})
            c0s = [rgba(c) for c in regs.get(0, [(0, 0, 0, 255)] * 5)]
            c2s = [rgba(c) for c in regs.get(2, [c2 + (255,)] * 5)]
            out_groups.append((texture(tex_list[tex]) if tex is not None else 0xFFFFFFFF, first + first_index, count,
                               kind | (cull << 8), part, mult, c0s, c2s))
    add_groups(groups, texs, 0, body_brk, 0, 0)
    verts, indices = list(verts), list(indices)
    parts = []
    for k, (pname, joint, brk_idx) in enumerate(spec["parts"]):
        (_, _, _, pverts, pidx, pgroups, _, _), ptexs = model(pname)
        to_model = inv(inv_bind[joint])  # the joint's bind pose: the part's vertices, in its joint's space, into the model's
        base = len(verts)
        for p, n, t, sk in pverts:
            verts.append((tuple(xf(to_model, p)), tuple(xf(to_model, n, 0)), t, ((joint, 1.0),)))
        first = len(indices)
        indices += [base + i for i in pidx]
        regs = {}
        if brk_idx >= 0:
            b = brk(kBrk[brk_idx])
            regs = {m: v for m, v in b.items()}
        add_groups(pgroups, ptexs, k + 1, regs, first, base)
        parts.append((pname, joint))
    assert len(verts) < 65536
    anims = []
    afiles = files if spec["anims"] == spec["arc"] else archive(spec["anims"])
    for path in sorted(afiles):
        if path.endswith(".bck"):
            loop, frames = load_bck(afiles[path])
            if len(frames[0]) != len(jnames):  # another model's (tnb_sword_b_pull_*: the sword's own 3 joints)
                print("  %s: %d joints, not the body's; left out" % (path, len(frames[0])))
                continue
            anims.append((os.path.basename(path)[:-4], loop, frames))
    out = bytearray(b"SBST" + struct.pack("<I", 2))
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
    out += struct.pack("<I", len(out_groups))
    for tex, first, count, flags, part, mult, c0s, c2s in out_groups:
        out += struct.pack("<5If10I", tex, first, count, flags, part, mult, *c0s, *c2s)
    out += struct.pack("<I", len(parts))
    for pname, joint in parts: out += struct.pack("<32si", pname.encode(), joint)
    out += struct.pack("<I", len(textures))
    for w, h, px in textures: out += struct.pack("<2I", w, h) + px
    out += struct.pack("<I", len(anims))
    for aname, loop, frames in anims:
        out += struct.pack("<32s2I", aname.encode(), len(frames), loop)
        for row in frames:
            for S, Q, T in row: out += struct.pack("<10f", *S, *Q, *T)
    path = os.path.join(OUT, "tp_%s.bin" % name)
    open(path, "wb").write(out)
    lo = [min(v[0][i] for v in verts) for i in range(3)]
    hi = [max(v[0][i] for v in verts) for i in range(3)]
    print("%s: %d joints, %d parts, %d vertices, %d triangles, %d textures, %d animations -> %s (%.1f MB)" % (
        name, len(jnames), len(parts), len(verts), len(indices) // 3, len(textures), len(anims), path, len(out) / 1e6))
    print("  bind pose bounds", [round(x) for x in lo], [round(x) for x in hi])
    print("  joints", " ".join("%d:%s<%d" % (i, n, p) for i, (n, p) in enumerate(zip(jnames, parent))))
    print("  animations", " ".join("%s:%d%s" % (n, len(f), "L" if l else "") for n, l, f in anims))


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, spec in SPECS.items(): build(name, spec)


if __name__ == "__main__":
    main()
