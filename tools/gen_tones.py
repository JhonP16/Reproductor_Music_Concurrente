#!/usr/bin/env python3
"""Genera WAVs de prueba con formatos variados (solo biblioteca estándar).

Cada archivo usa una frecuencia de muestreo, número de canales y
profundidad distintos para ejercitar el parser WAV y la reconfiguración
de ALSA entre pistas consecutivas.

    python3 tools/gen_tones.py assets
"""
import math
import os
import struct
import sys

NOTES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def freq(note, octave):
    return 440.0 * 2 ** ((NOTES[note] + 12 * (octave - 4) - 9) / 12)


def synth(rate, seconds, melody, chord):
    """Arpegio + acorde de fondo + bombo sencillo, valores en [-1, 1]."""
    n = int(rate * seconds)
    beat = rate * 60 // 120 // 2  # corcheas a 120 bpm
    out = []
    for i in range(n):
        t = i / rate
        step = (i // beat) % len(melody)
        f = melody[step]
        local = (i % beat) / rate
        env = math.exp(-local * 6)
        s = 0.35 * env * (math.sin(2 * math.pi * f * t)
                          + 0.3 * math.sin(4 * math.pi * f * t))
        s += 0.12 * sum(math.sin(2 * math.pi * c * t) for c in chord) / len(chord)
        if (i // beat) % 4 == 0:  # bombo
            kick_t = local
            s += 0.5 * math.exp(-kick_t * 25) * math.sin(2 * math.pi * 55 * kick_t * (1 + 2 * math.exp(-kick_t * 40)))
        out.append(max(-1.0, min(1.0, s)))
    return out


def write_wav(path, rate, channels, bits, samples, fmt_float=False):
    frames = bytearray()
    for i, s in enumerate(samples):
        pan = 0.5 + 0.4 * math.sin(2 * math.pi * 0.25 * i / rate)
        chans = [s * (1 - pan) * 2, s * pan * 2] if channels == 2 else [s]
        for v in chans:
            v = max(-1.0, min(1.0, v))
            if fmt_float:
                frames += struct.pack("<f", v)
            elif bits == 8:
                frames += struct.pack("<B", int(v * 127 + 128))
            elif bits == 16:
                frames += struct.pack("<h", int(v * 32767))
            elif bits == 24:
                frames += struct.pack("<i", int(v * 8388607))[:3]
    block = channels * bits // 8
    fmt_tag = 3 if fmt_float else 1
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, fmt_tag, channels, rate,
                                       rate * block, block, bits))
        f.write(b"data" + struct.pack("<I", len(frames)) + frames)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "assets"
    os.makedirs(out, exist_ok=True)
    tracks = [
        ("01_Arpegio_en_Do.wav", 44100, 2, 16, False,
         [freq(n, 4) for n in "CEGCEGBG"], [freq("C", 3), freq("G", 3)]),
        ("02_Menor_Melancolico.wav", 48000, 2, 24, False,
         [freq(n, 4) for n in "ACEACEGE"], [freq("A", 2), freq("E", 3)]),
        ("03_Lofi_Mono_8bit.wav", 22050, 1, 8, False,
         [freq(n, 5) for n in "DFADFAGF"], [freq("D", 3)]),
        ("04_Flotante_32.wav", 44100, 2, 32, True,
         [freq(n, 4) for n in "GBDGBDFD"], [freq("G", 2), freq("D", 3)]),
        ("05_Final_Brillante.wav", 32000, 2, 16, False,
         [freq(n, 5) for n in "EGBEGBDB"], [freq("E", 3), freq("B", 3)]),
    ]
    for name, rate, ch, bits, flt, mel, chord in tracks:
        path = os.path.join(out, name)
        write_wav(path, rate, ch, bits, synth(rate, 20, mel, chord), flt)
        print(f"  {path}: {rate} Hz, {ch} canal(es), {bits} bits{' float' if flt else ''}")


if __name__ == "__main__":
    main()
