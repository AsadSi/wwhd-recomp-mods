"""Cuts Twilight Princess HD's own sound effects out of a recording made by the TP HD recomp's sound recorder into one
WAV each, for the Twilight Princess monsters (game/tp_sounds/<id>.wav, read by runtime/src/mods/shadow_beast.cpp).

Record them first, in ~/Documents/code/tphd-recomp (its test aid runtime/src/mods/sfx_record.cpp): a QA save, warped
into Link's house (quiet), the music faded out, the enemy test stage's SE waves loaded, each id played in turn:

    D=build/audit/saves/sfxrec; mkdir -p $D/user; cp game/content/DebugSaves/QASaves/ZTP01.dat $D/user/ZTP00.dat
    SAVE=sfxrec WWHD_WARP=4500:R_SP01,4,4,0 WWHD_SFX_START=900 WWHD_SFX_WAVES=<waves, as kIds says> WWHD_NO_AUDIO=1 \\
      WWHD_AUDIO_DUMP=<rec.wav> WWHD_SFX_RECORD=$(tools/tp_sounds.py ids beast) tools/audit/run_route.sh <empty route> 275

    tools/tp_sounds.py <rec.wav> <tphd-recomp>/build/audit/sfxrec/log.txt [<out dir>]

Each effect starts at its "[sfxrec] <id> at <frame>" mark (searched for from a quarter second before it, the mix runs
ahead of the log) and ends where it falls back to the room's noise, at most where the recorder stopped every effect
(2.8 s on)."""
import os, re, struct, sys
import numpy as np

# the sound effect ids, as WWHD_SFX_RECORD takes them: the Shadow Beast's (d_a_e_s1.cpp: Z2SE_EN_NS_*, HIT_WOLFBITE;
# WWHD_SFX_WAVES=8A,8B, the enemy test stage's) and the Darknut's (d_a_b_tn.cpp: Z2SE_EN_TN_*, CM_BODYFALL_M; the
# monsters' burst JA_SE_CM_MONS_EXPLODE and the twilight's DARK_VANISH; WWHD_SFX_WAVES=15,17,21,22, the Temple of
# Time's and Hyrule Castle's)
kIds = {"beast": [0x7000D, 0x7000E, 0x7000F, 0x70010, 0x70011, 0x700D3, 0x700D4, 0x700FA, 0x700FB, 0x700FC, 0x700FD, 0x70115,
                  0x70116, 0x70118, 0x70119, 0x701B3, 0x70222, 0x70223, 0x70224, 0x70225, 0x40003, 0x6002C],
        "darknut": list(range(0x70399, 0x703B8)) + [0x703E5, 0x703E6, 0x703E7, 0x703E8, 0x60028, 0x6000A]}

if __name__ == "__main__" and len(sys.argv) == 3 and sys.argv[1] == "ids":  # tools/tp_sounds.py ids <beast|darknut>
    print(",".join("%X" % i for i in kIds[sys.argv[2]]))
    sys.exit()

rec, log = sys.argv[1], sys.argv[2]
out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(__file__), "..", "game", "tp_sounds")
d = open(rec, "rb").read()
a = np.frombuffer(d[44:44 + (len(d) - 44) // 4 * 4], "<i2").reshape(-1, 2).astype(np.float32)
marks = [(m.group(1), int(m.group(2))) for m in (re.search(r"\[sfxrec\] (\w+) at (\d+)", l) for l in open(log)) if m]
marks.append(("end", len(a)))
win = 480  # 10 ms
def env(s, e): return np.array([np.sqrt((a[i:i + win] ** 2).mean()) for i in range(max(0, s), max(s, e) - win, win)])
floor = np.median(env(marks[0][1] - 48000, marks[0][1]))
os.makedirs(out, exist_ok=True)
for (name, s), (_, nxt) in zip(marks, marks[1:]):
    if name == "done": continue
    lo = s - 12000  # a quarter second before
    e = env(lo, min(nxt - 12000, s + int(2.85 * 48000)))
    loud = np.nonzero(e > max(4 * floor, 150))[0]
    if not len(loud):
        print("%s: silent" % name)
        continue
    start = lo + loud[0] * win
    end = lo + (loud[-1] + 1) * win
    clip = a[start:end].mean(1)
    fade = min(len(clip) // 4, 480)
    clip[-fade:] *= np.linspace(1, 0, fade)
    pcm = np.clip(clip, -32768, 32767).astype("<i2").tobytes()
    with open(os.path.join(out, "%s.wav" % name.upper()), "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 48000, 96000, 2, 16)
                + b"data" + struct.pack("<I", len(pcm)) + pcm)
    print("%s: %.2f s, peak %.0f" % (name, len(clip) / 48000, np.abs(clip).max()))
