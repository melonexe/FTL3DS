"""Generates the CIA's home-menu icon (48x48), banner (256x128) and banner sound: a starfield,
a small ship and pixel lettering."""
import os
import random
import struct
import wave
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

FONT = {  # 5x7 pixel letters
    'F': ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    'T': ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    'L': ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    '3': ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    'D': ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    'S': ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    ' ': ["00000"] * 7,
}


def write_png(path, w, h, px):
    raw = b''.join(b'\0' + bytes(c for p in px[y * w:(y + 1) * w] for c in p) for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def canvas(w, h, seed):
    rnd = random.Random(seed)
    px = [(int(8 + 20 * y / h), int(10 + 14 * y / h), int(28 + 30 * y / h), 255) for y in range(h) for x in range(w)]
    for _ in range(w * h // 40):
        x, y = rnd.randrange(w), rnd.randrange(h)
        b = rnd.choice([140, 180, 230])
        px[y * w + x] = (b, b, min(255, b + 20), 255)
    return px


def put(px, w, x, y, c):
    if 0 <= x < w and 0 <= y < len(px) // w:
        px[y * w + x] = c


def text(px, w, s, x0, y0, scale, col):
    for i, ch in enumerate(s):
        for gy, row in enumerate(FONT[ch]):
            for gx, bit in enumerate(row):
                if bit == '1':
                    for dy in range(scale):
                        for dx in range(scale):
                            put(px, w, x0 + i * 6 * scale + gx * scale + dx, y0 + gy * scale + dy, col)


def ship(px, w, cx, cy, s):
    hull, dark, glow = (190, 196, 205, 255), (90, 96, 110, 255), (255, 150, 60, 255)
    for y in range(-3 * s, 3 * s + 1):
        half = int((12 * s) * (1 - abs(y) / (3.6 * s)))
        for x in range(-12 * s, -12 * s + 2 * half):
            put(px, w, cx + x, cy + y, hull if abs(y) < 2 * s else dark)
    for y in range(-s, s + 1):
        for x in range(-15 * s, -12 * s):
            put(px, w, cx + x, cy + y, glow)
    for i in range(4):
        put(px, w, cx - 6 * s + i * 3 * s, cy, (120, 220, 255, 255))


def main():
    icon = canvas(48, 48, 1)
    ship(icon, 48, 26, 32, 1)
    text(icon, 48, "FTL", 6, 6, 2, (255, 215, 90, 255))
    write_png(os.path.join(HERE, "icon.png"), 48, 48, icon)
    banner = canvas(256, 128, 2)
    ship(banner, 256, 150, 84, 3)
    text(banner, 256, "FTL 3DS", 28, 18, 5, (255, 215, 90, 255))
    write_png(os.path.join(HERE, "banner.png"), 256, 128, banner)
    with wave.open(os.path.join(HERE, "banner.wav"), 'wb') as wv:
        wv.setnchannels(1)
        wv.setsampwidth(2)
        wv.setframerate(22050)
        wv.writeframes(b'\0\0' * 22050)
    print("ok")


if __name__ == "__main__":
    main()
