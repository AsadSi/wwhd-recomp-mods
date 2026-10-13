"""Twilight Princess HD's particle effects for its monsters in Wind Waker HD -> game/tp_particles.bin, the emitters
(their JPA2 parameters, as TP's JParticle reads them) and textures runtime/src/mods/tp_fx.cpp simulates and
gfx/beast.mm draws. TP's .jpc (JPAC2-10) are another JParticle than WW's (JPAC1-00), so WW's own particle system
cannot load them.

    tools/tp_particles.py [<TP HD game dir>] [<out>]   (defaults ~/Documents/code/tphd-recomp/game, game/tp_particles.bin)

An effect id with bit 0x8000 is the scene bank's (Pscene*.jpc), else common-r's (d_particle.cpp's getRM_ID); the
textures are the .jpc's TEX1 stubs, their pixels the n-th surface of content/tex/Particle/<file>.gtx.gz.

Output, little-endian: 'TPFX' u32 version, u32 textures, each (u32 w, h, RGBA8), u32 emitters, each u32 id then
kFloats f32 (the layout is Emitter in tp_fx.cpp)."""
import gzip, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tp_monsters as sb  # its GTX decoder  # noqa: E402

TP = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Documents/code/tphd-recomp/game")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "game", "tp_particles.bin")
# the effects, by the file they come from: the Shadow Beast's body (0x387, d_a_e_s1.cpp body_eff_set) and its twilight
# dissolve (fopAcM_createDisappear type 1: 0x826C-0x826E, d_a_disappear.cpp); the Darknut's (d_a_b_tn.cpp: sparks off
# its slam 0x87DA-0x87DD, armour breaking 0x87DE-0x87E0, a piece landing 0x880B) and the monsters' burst it dies in
# (createDisappear type 0: 0x108-0x10C; 0x154/0x155 keyframed, left out)
kEffects = {"common-r": [0x387, 0x108, 0x109, 0x10A, 0x10B, 0x10C], "Pscene010": [0x826C, 0x826D, 0x826E],
            "Pscene150": [0x87DA, 0x87DB, 0x87DC, 0x87DD, 0x87DE, 0x87DF, 0x87E0, 0x880B]}
kFloats = 96


def parse(d):
    assert d[:8] == b"JPAC2-10", d[:8]
    nres, ntex, toff = struct.unpack(">HHI", d[8:16])
    o, res = 0x10, {}
    for _ in range(nres):
        uid, nb = struct.unpack(">HH", d[o:o + 4])
        o += 8
        blocks = {}
        for _ in range(nb):
            m, sz = d[o:o + 4].decode(), struct.unpack(">I", d[o + 4:o + 8])[0]
            blocks.setdefault(m, []).append(d[o:o + sz])
            o += sz
        res[uid] = blocks
    names, o = [], toff
    for _ in range(ntex):
        sz = struct.unpack(">I", d[o + 4:o + 8])[0]
        names.append(d[o + 0xC:o + 0x20].split(b"\0")[0].decode())
        o += sz
    return res, names


def f(b, o, n=1): return list(struct.unpack(">%df" % n, b[o:o + 4 * n]))
def u(b, o, fmt): return list(struct.unpack(">" + fmt, b[o:o + struct.calcsize(fmt)]))


def emitter(blocks, tex_base):
    """One emitter's parameters as kFloats floats (Emitter in tp_fx.cpp)."""
    e = blocks["BEM1"][0]
    fl = u(e, 8, "I")[0]
    v = [float((fl >> 8) & 7), float(fl & 1), float((fl >> 1) & 1)]          # 0 volume, fixed density, fixed interval
    v += f(e, 0x10, 3) + f(e, 0x1C, 3)                                       # 3 scale, 6 translation
    d = f(e, 0x28, 3)
    n = math.sqrt(sum(x * x for x in d)) or 1
    v += [x / n for x in d]                                                  # 9 direction
    v += f(e, 0x34, 6)                                                       # 12 velOmni axis rndm dir spread ratio
    v += f(e, 0x4C, 7)                                                       # 18 rate rateRndm lifeRndm sweep minRad air moment
    maxf, start, life, vol, div = u(e, 0x6E, "hhhHH")
    v += [float(maxf), float(start), float(life), float(vol), float(div)]    # 25 max frame, start, life, volume size, div
    s = blocks["BSP1"][0]
    sfl = u(s, 8, "I")[0]
    po, _eo = u(s, 0xC, "hh")
    bm, ac, r0 = u(s, 0x18, "HBB")
    tf, tn, ti, cf, pn = u(s, 0x1E, "BBBBB")
    tdb = u(blocks["TDB1"][0], 8, "H" * ((len(blocks["TDB1"][0]) - 8) // 2)) if "TDB1" in blocks else [0]
    v += [float(sfl & 15), float((sfl >> 4) & 7)] + f(s, 0x10, 2)            # 30 shape, direction type, 32 size x, y
    v += [float(bm & 3), r0 / 255.0, float((sfl >> 15) & 7), float((sfl >> 18) & 1)]  # 34 blend, alpha ref, colour, A0 only
    v += [c / 255.0 for c in u(s, 0x26, "BBBB")] + [c / 255.0 for c in u(s, 0x2A, "BBBB")]  # 38 prm rgba, 42 env rgba
    keys = [u(s, po + 6 * i, "hBBBB") for i in range(pn)] if cf & 2 else []
    keys = (keys + [keys[-1]] * 4)[:4] if keys else []
    v += [float(len(keys))]                                                  # 46 prm keys
    for k in keys or [[0, 0, 0, 0, 0]] * 4: v += [float(k[0])] + [c / 255.0 for c in k[1:]]  # 47 4 x (frame, rgba)
    v += [float(tex_base + tdb[ti if ti < len(tdb) else 0])]                 # 67 texture
    x = blocks.get("ESP1", [None])[0]
    if x:
        xf = u(x, 8, "I")[0]
        v += [float(xf & 1)] + f(x, 0xC, 1) + f(x, 0x14, 1) + f(x, 0x1C, 1) + f(x, 0x10, 1) + f(x, 0x18, 1) + f(x, 0x20, 1) + f(x, 0x24, 1)
        v += [float((xf >> 16) & 1)] + f(x, 0x2C, 1) + f(x, 0x34, 1) + f(x, 0x38, 1) + f(x, 0x30, 1) + f(x, 0x3C, 1)
        v += [float((xf >> 24) & 1)] + f(x, 0x50, 1) + f(x, 0x54, 1) + f(x, 0x58, 1)
    else:
        v += [0.0] * 18
    # 68 scale anm, in t, in x, in y, out t, out x, out y, rndm; 76 alpha anm, in t, in v, base, out t, out v;
    # 82 rot anm, rot rndm, speed, speed rndm
    flds = blocks.get("FLD1", [])[:2]
    for fb in flds: v += [float(u(fb, 8, "I")[0] & 15)] + f(fb, 0x18, 3) + f(fb, 0x24, 1)  # 86 2 x (type, dir, mag)
    v += [-1.0, 0, 0, 0, 0] * (2 - len(flds))
    assert len(v) == kFloats, len(v)
    return v


def main():
    out_tex, out_em = [], []
    for name, ids in kEffects.items():
        res, names = parse(open(os.path.join(TP, "content/res/Particle/%s.jpc" % name), "rb").read())
        gtx = gzip.decompress(open(os.path.join(TP, "content/tex/Particle/%s.jpc.gtx.gz" % name), "rb").read())
        texs = sb.gtx_textures(gtx)
        assert len(texs) == len(names), (name, len(texs), len(names))
        base = len(out_tex)
        out_tex += texs
        for i in ids:
            out_em.append((i, emitter(res[i], base)))
            print("%s %#x: texture %s" % (name, i, names[int(out_em[-1][1][67]) - base]))
    # keep only the textures used
    used = sorted({int(v[67]) for _, v in out_em})
    remap = {t: k for k, t in enumerate(used)}
    out = bytearray(b"TPFX" + struct.pack("<I", 1) + struct.pack("<I", len(used)))
    for t in used:
        w, h, px = out_tex[t]
        out += struct.pack("<2I", w, h) + px
    out += struct.pack("<I", len(out_em))
    for i, v in out_em:
        v[67] = float(remap[int(v[67])])
        out += struct.pack("<I%df" % kFloats, i, *v)
    open(OUT, "wb").write(out)
    print("%d emitters, %d textures -> %s (%d bytes)" % (len(out_em), len(used), OUT, len(out)))


if __name__ == "__main__":
    main()
