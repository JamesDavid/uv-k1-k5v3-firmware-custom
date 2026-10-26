#!/usr/bin/env python3
"""Upload a OnePress word model to a UV-K1 / UV-K5 V3 running the OnePress firmware.

    pip install pyserial
    python tools/onepress_upload.py COM5 onepress-s.bin        (Linux: /dev/ttyACM0)

The model (onepress-s.bin, 246 KB, from github.com/JamesDavid/OnePress-Keyboard)
goes to external flash at 0x1A0000, where the OnePress apps' decoder reads it
(APP_OP_MODEL_ADDR in App/apps/app_api.h). Each 4 KiB sector is compared first
(CRC-32) and only rewritten when it differs, then checked again, so uploading
the same model twice changes nothing. It uses the firmware's raw external-flash
commands (0x0738-0x073E, ENABLE_FEAT_F4HWN_EXT_FLASH_RW), which never touch the
calibration sector. Turn the radio on (main screen, no app open), connect the
USB-C cable, run this. Restart the OnePress app afterwards.
"""
import argparse
import struct
import sys
import time
import zlib

import serial

MODEL_ADDR = 0x1A0000
MODEL_MAX = 0x3E000             # up to the Sonic 4K reserve at 0x1DE000
SECTOR = 0x1000
CHUNK = 128                     # PY25Q16_RAW_CHUNK_SIZE
OBFUS = bytes.fromhex("166c14e62e910d402135d5401303e980")
ERRORS = {1: "magic", 2: "version", 3: "not committed", 4: "size", 5: "CRC", 6: "SPI timeout",
          7: "slot", 8: "refused (handshake)", 9: "RAM load", 10: "protected"}


def crc16(b: bytes) -> int:
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021 if c & 0x8000 else c << 1) & 0xFFFF
    return c


def xor(b: bytes) -> bytes:
    return bytes(x ^ OBFUS[i % 16] for i, x in enumerate(b))


class Radio:
    def __init__(self, port: str):
        self.ser = serial.Serial(port, 38400, timeout=0.05)
        self.rx = bytearray()
        self.ts = int(time.time()) & 0xFFFFFFFF

    def send(self, msg_id: int, data: bytes):
        msg = struct.pack("<HH", msg_id, len(data)) + data
        if len(msg) & 1:
            msg += b"\0"
        body = msg + struct.pack("<H", crc16(msg))
        self.ser.write(struct.pack("<HH", 0xCDAB, len(msg)) + xor(body) + struct.pack("<H", 0xBADC))

    def recv(self, msg_id: int, timeout: float = 3.0) -> bytes:
        end = time.time() + timeout
        while time.time() < end:
            self.rx += self.ser.read(512)
            while True:
                i = self.rx.find(b"\xab\xcd")
                if i < 0 or len(self.rx) - i < 8:
                    break
                n = self.rx[i + 2] | self.rx[i + 3] << 8
                if len(self.rx) - i < 8 + n:
                    break
                if self.rx[i + 6 + n:i + 8 + n] != b"\xdc\xba":
                    del self.rx[:i + 2]
                    continue
                msg = xor(self.rx[i + 4:i + 6 + n])[:n]
                del self.rx[:i + 8 + n]
                if len(msg) >= 4 and (msg[0] | msg[1] << 8) == msg_id:
                    return msg[4:]
        raise TimeoutError(f"no reply {msg_id:#06x} from the radio")

    def hello(self) -> str:
        self.send(0x0514, struct.pack("<I", self.ts))
        r = self.recv(0x0515)
        return r[:16].split(b"\0")[0].decode("ascii", "replace")

    @staticmethod
    def check(status: int, what: str):
        if status:
            raise RuntimeError(f"{what}: {ERRORS.get(status, status)}")

    def crc(self, addr: int, n: int) -> int:
        self.send(0x073E, struct.pack("<III", addr, n, self.ts))
        a, size, crc, st = struct.unpack("<IIIB", self.recv(0x073F)[:13])
        self.check(st, f"CRC at {addr:#x}")
        return crc

    def erase(self, addr: int):
        self.send(0x073A, struct.pack("<II", addr, self.ts))
        self.check(self.recv(0x073B)[4], f"erase at {addr:#x}")

    def write(self, addr: int, data: bytes):
        self.send(0x073C, struct.pack("<IHI", addr, len(data), self.ts) + data)
        self.check(self.recv(0x073D)[6], f"write at {addr:#x}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("port", help="the radio's USB serial port (COM5, /dev/ttyACM0)")
    ap.add_argument("model", help="the model file (onepress-s.bin)")
    a = ap.parse_args()
    data = open(a.model, "rb").read()
    if data[:4] != b"KPD2":
        sys.exit(f"{a.model} is not a OnePress model (KPD2)")
    if len(data) > MODEL_MAX:
        sys.exit(f"{a.model} is {len(data)} bytes: at most {MODEL_MAX} fit (use onepress-s.bin)")
    r = Radio(a.port)
    print(f"Radio: {r.hello()}")
    sectors = (len(data) + SECTOR - 1) // SECTOR
    written = 0
    for s in range(sectors):
        addr = MODEL_ADDR + s * SECTOR
        part = data[s * SECTOR:(s + 1) * SECTOR]
        want = zlib.crc32(part)
        if r.crc(addr, len(part)) != want:
            r.erase(addr)
            for o in range(0, len(part), CHUNK):
                r.write(addr + o, part[o:o + CHUNK])
            if r.crc(addr, len(part)) != want:
                sys.exit(f"\nsector {addr:#x} did not verify: try again")
            written += 1
        print(f"\r{s + 1}/{sectors} sectors ({written} rewritten)", end="", flush=True)
    print(f"\nDone: {len(data)} bytes at {MODEL_ADDR:#x}. Restart the OnePress app.")


if __name__ == "__main__":
    main()
