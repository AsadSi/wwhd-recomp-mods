#!/usr/bin/env python3
"""Edit a portable save state: edit_state.py IN.wwstate OUT.wwstate key=value ... (a text key such as link_pos or
   on_ship), sd:<block>.<field>=hex / sd:<block>+off=hex (the save data, blocks and fields as wwsave.py names them),
   ev=XXYY+XXYY (event bits to set). Recomputes the save-data checksum and the file CRC. Plain Python 3."""
import os, re, sys, zlib
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "savegame"))
import wwsave as W
src, dst, *edits = sys.argv[1:]
text = open(src).read()
at = text.find("\nchecksum = "); body = text[:at + 1]
sd = bytearray(bytes.fromhex(re.search(r"^savedata = (\w+)", body, re.M).group(1)))
blocks = {n: (o, s) for n, o, s in W._HD_BLOCKS}
for e in edits:
    k, v = e.split("=", 1)
    if k.startswith("sd:"):
        spec = k[3:]
        if "+" in spec:
            blk, off = spec.split("+"); o = blocks[blk][0] + int(off, 0)
        else:
            blk, fld = spec.split("."); o = blocks[blk][0] + {n: off for n, off, s in W._FIELDS[blk]}[fld]
        b = bytes.fromhex(v); sd[o:o + len(b)] = b
    elif k == "ev":
        o = blocks["event"][0]
        for bit in v.split("+"):
            if not bit: continue
            n = int(bit, 16); sd[o + (n >> 8)] |= n & 0xFF
    else:
        body = re.sub(r"^%s = .*$" % re.escape(k), "%s = %s" % (k, v), body, count=1, flags=re.M)
import struct
a, b = W.hd_slot_checksum(sd); sd[W.HD_SLOT_USED:W.HD_SLOT_USED + 8] = struct.pack(">II", a, b)
body = re.sub(r"^savedata = \w+$", "savedata = " + sd.hex(), body, count=1, flags=re.M)
open(dst, "w").write(body + "checksum = crc32:%08X\n" % (zlib.crc32(body.encode()) & 0xFFFFFFFF))
print("wrote", dst)
