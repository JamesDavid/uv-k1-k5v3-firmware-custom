#!/usr/bin/env python3
"""RX audio of an MCW transmission for the background CW receiver test
(App/onepress/cw_rx.c): squelch-closed noise, then the carrier (the noise
quieted) with a tone keyed in Morse, then noise again, as 12-bit ADC codes
at 9.6 kHz around 2048. Writes <out>.u16 (little-endian).

  cwgen.py out "CQ CQ DE KE7AQA K" [--wpm 18] [--tone 700] [--level 300]
           [--hiss 8] [--noise 150] [--jitter 0.1] [--seed 1]

--level is the tone's amplitude in ADC codes, --hiss the noise left under the
carrier, --noise the squelch-closed noise (standard deviations); --jitter
varies each element's length by up to that fraction (a hand key).
"""
import argparse, math, random

MORSE = {'A': '.-', 'B': '-...', 'C': '-.-.', 'D': '-..', 'E': '.', 'F': '..-.', 'G': '--.', 'H': '....',
         'I': '..', 'J': '.---', 'K': '-.-', 'L': '.-..', 'M': '--', 'N': '-.', 'O': '---', 'P': '.--.',
         'Q': '--.-', 'R': '.-.', 'S': '...', 'T': '-', 'U': '..-', 'V': '...-', 'W': '.--', 'X': '-..-',
         'Y': '-.--', 'Z': '--..', '0': '-----', '1': '.----', '2': '..---', '3': '...--', '4': '....-',
         '5': '.....', '6': '-....', '7': '--...', '8': '---..', '9': '----.', '/': '-..-.', '?': '..--..'}
FS = 9600


def keying(text, wpm, jitter, rnd):
    """(down, seconds) pairs."""
    dit = 1.2 / wpm
    j = lambda units: units * dit * (1 + rnd.uniform(-jitter, jitter))
    out = []
    for word in text.upper().split():
        for ch in word:
            for s in MORSE[ch]:
                out += [(True, j(1 if s == '.' else 3)), (False, j(1))]
            out[-1] = (False, j(3))
        out[-1] = (False, j(7))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("text")
    ap.add_argument("--wpm", type=float, default=18)
    ap.add_argument("--tone", type=float, default=700)
    ap.add_argument("--level", type=float, default=300)
    ap.add_argument("--hiss", type=float, default=8)
    ap.add_argument("--noise", type=float, default=150)
    ap.add_argument("--jitter", type=float, default=0.1)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    x = [rnd.gauss(0, a.noise) for _ in range(int(0.5 * FS))]            # squelch closed
    x += [rnd.gauss(0, a.hiss) for _ in range(int(0.3 * FS))]            # the carrier
    ph, ramp = 0.0, int(0.004 * FS)                                       # 4 ms edges
    for down, sec in keying(a.text, a.wpm, a.jitter, rnd):
        n = int(sec * FS)
        for i in range(n):
            g = min(1.0, i / ramp, (n - i) / ramp) if down else 0.0
            ph += 2 * math.pi * a.tone / FS
            x.append(a.level * g * math.sin(ph) + rnd.gauss(0, a.hiss))
    x += [rnd.gauss(0, a.hiss) for _ in range(int(0.6 * FS))]
    x += [rnd.gauss(0, a.noise) for _ in range(int(0.5 * FS))]
    with open(a.out + ".u16", "wb") as f:
        f.write(b"".join(min(4095, max(0, int(round(2048 + v)))).to_bytes(2, "little") for v in x))


if __name__ == "__main__":
    main()
