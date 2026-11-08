#!/usr/bin/env python3
"""Radio audio for the background APRS receiver test (App/onepress/aprs_rx.c).

Builds APRS frames, makes the RX audio the K1 samples on PA4 with the APRS RX
app's channel model (App/apps/aprsrx/test/model_rx.py: a real station's sine
AFSK, the radio's de-emphasis and filters, noise), then samples it at 9.6 kHz
the way a timer interrupt would: each sample late by a random 0..jitter us.
Writes <out>.u16 (little-endian ADC codes) and <out>.txt (what was sent).

  gen.py out [--jitter US] [--noise HZ] [--twist DB] [--mode std|raw] [--seed N] [--scenario NAME]
"""
import argparse, math, os, random, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "App", "apps", "aprsrx", "test"))
import model_rx as mr
from ax25 import build

ME = "KE7AQA-7"
SCENARIOS = {
    # (source, path, info): to me, to someone else, a position, to me again
    "traffic": [
        ("W1ABC-9", ("WIDE1-1", "WIDE2-1"), ":KE7AQA-7 :Where are you?{12"),
        ("N0XYZ", ("WIDE2-1",), ":W1ABC-9  :not for you{3"),
        ("K7POS-7", ("WIDE1-1",), "!4903.50N/07201.75W>moving"),
        ("W1ABC-9", ("WIDE1-1", "WIDE2-1"), ":KE7AQA-7 :Where are you?{12"),
        ("AB1CD", ("WIDE1-1", "WIDE2-2"), ":KE7AQA-7 :73 see you at the hamfest tomorrow, bring the radio and spare batts"),
    ],
    "one": [("W1ABC-9", ("WIDE1-1", "WIDE2-1"), ":KE7AQA-7 :Where are you?{12")],
    # to the simulator's station (tools/onepress_sim: the boot callsign N0CALL, no SSID)
    "sim": [("W1ABC-9", ("WIDE1-1", "WIDE2-1"), ":N0CALL   :Where are you?{12")],
}


def audio(frames, mode, noise, twist, seed):
    """The discriminator output (Hz, at 96 kHz) through the radio's audio path."""
    rnd = random.Random(seed)
    gap = int(0.15 * mr.FS_SIM)
    x = []
    for f in frames:
        x += [rnd.gauss(0, 6000) for _ in range(gap)]          # squelch-closed noise between
        x += [v + rnd.gauss(0, noise) for v in mr.sine_wave(f, twist_db=twist)]
    x += [rnd.gauss(0, 6000) for _ in range(gap)]
    if mode == "raw":
        return mr.rc_hp(mr.biquad_lp(x, 5000), 30)
    return mr.biquad_lp(mr.deemph(mr.rc_hp(x, 300)), 3000)


def sample(x, jitter_us, seed):
    """9.6 kHz ADC codes, each conversion late by a uniform 0..jitter_us."""
    rnd = random.Random(seed + 1000)
    out, n = [], 0
    while True:
        p = (n / mr.FS_ADC + rnd.uniform(0, jitter_us) * 1e-6) * mr.FS_SIM
        i = int(p)
        if i + 1 >= len(x):
            return out
        v = x[i] * (1 - (p - i)) + x[i + 1] * (p - i)
        out.append(min(4095, max(0, int(round(mr.BIAS + mr.LSB_PER_HZ * v + rnd.gauss(0, 1))))))
        n += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--jitter", type=float, default=0.0)
    ap.add_argument("--noise", type=float, default=1500.0)
    ap.add_argument("--twist", type=float, default=0.0)
    ap.add_argument("--mode", default="std")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--scenario", default="traffic")
    a = ap.parse_args()
    frames = [build(src, "APZOP", path, info) for src, path, info in SCENARIOS[a.scenario]]
    adc = sample(audio(frames, a.mode, a.noise, a.twist, a.seed), a.jitter, a.seed)
    with open(a.out + ".u16", "wb") as f:
        f.write(b"".join(v.to_bytes(2, "little") for v in adc))
    with open(a.out + ".txt", "w") as f:
        for src, path, info in SCENARIOS[a.scenario]:
            f.write(f"{src}>{','.join(path)}:{info}\n")


if __name__ == "__main__":
    main()
