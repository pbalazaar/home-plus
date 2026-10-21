#!/usr/bin/env python3
"""Writes native/qml/sounds/tick.wav and select.wav: Home+'s focus sounds (native/src/sound.c: synth,
the same numbers), for the Control Center over other apps, which LG's compositor plays.
python3 tools/make-sounds.py
"""
import math
import os
import struct
import wave

RATE = 48000
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def synth(ms, f1, f2, decay_ms, noise, gain):
    """A decaying two-tone ping with a short noise transient on top; a 0.5 ms attack keeps the start from clicking."""
    n, seed, out = int(RATE * ms / 1000), 2463534242, []
    for i in range(n):
        t = i / RATE
        env = math.exp(-t * 1000 / decay_ms) * min(1, t * RATE / 24)
        seed ^= (seed << 13) & 0xffffffff
        seed ^= seed >> 17
        seed ^= (seed << 5) & 0xffffffff
        nz = (seed & 0xffff) / 32768.0 - 1
        v = math.sin(2 * math.pi * f1 * t) * 0.7 + math.sin(2 * math.pi * f2 * t) * 0.3
        v = (v + nz * noise * math.exp(-t * 1000 / 2.0)) * env * gain
        out.append(int(max(-1, min(1, v)) * 32767))
    return out


def write(name, samples):
    path = os.path.join(ROOT, 'native/qml/sounds', name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(struct.pack('<%dh' % len(samples), *samples))
    print('wrote', os.path.relpath(path, ROOT))


write('tick.wav', synth(30, 2600, 5200, 4.5, 0.35, 0.16))
write('select.wav', synth(90, 880, 1320, 22, 0.15, 0.2))
