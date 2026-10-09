"""
Generates assets/sounds/wind.wav: a quiet, seamlessly looping outdoor wind bed.

    python tools/make_ambience.py

Brown-ish noise, low-passed, with slow overlapping gusts. The last second is
crossfaded into the first so the loop has no seam. Pure Python, no packages.
"""
import math
import os
import random
import struct
import wave

RATE = 22050
SECONDS = 12.0
FADE = 1.5
OUT = os.path.join(os.path.dirname(__file__), "..", "assets", "sounds", "wind.wav")

random.seed(7)
n = int(RATE * (SECONDS + FADE))
samples = []
brown = 0.0
lp1 = lp2 = 0.0
for i in range(n):
    t = i / RATE
    brown = brown * 0.985 + random.uniform(-1, 1) * 0.15      # brown noise: low rumble
    white = random.uniform(-1, 1)
    # gusts: a few slow sines that never quite line up
    gust = 0.55 + 0.25 * math.sin(t * 0.37 * 2 * math.pi) + 0.15 * math.sin(t * 0.13 * 2 * math.pi + 1.7) \
        + 0.1 * math.sin(t * 0.71 * 2 * math.pi + 0.4)
    # the "whistle" band rises with the gusts
    cutoff = 0.02 + 0.05 * gust
    lp1 += (white - lp1) * cutoff
    lp2 += (lp1 - lp2) * cutoff
    samples.append(brown * 0.6 + lp2 * 2.2 * gust)

# Seamless loop: crossfade the tail over the head
body = int(RATE * SECONDS)
fade = int(RATE * FADE)
for i in range(fade):
    k = i / fade
    samples[i] = samples[i] * k + samples[body + i] * (1 - k)
samples = samples[:body]

peak = max(abs(s) for s in samples)
scale = 0.6 / peak
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with wave.open(OUT, "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, s * scale)) * 32767)) for s in samples))
print("wrote", os.path.abspath(OUT), os.path.getsize(OUT), "bytes")
