#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
"""Long-audio pipeline test: silero VAD segmentation + emission-anchored
windowing (the exact sherpa_live live algorithm) on long_test.wav, then
score against phrases.list.

This exercises the FULL live segmentation path -- not just the recognizer
(per-phrase direct decode does that).  Usage:

  host>  python3 segment_and_score.py long_test.wav          # -> windows/
  host>  adb push windows <board>/tts_windows
  board> sherpa-onnx-offline ... decode each window/*.wav    # score output
"""
import sys, os
import numpy as np
import onnxruntime as ort

VAD = os.path.join(os.path.dirname(__file__), "..", "sherpa", "lib", "silero_vad.onnx")
THRESHOLD = 0.2            # same as sherpa_live
TAIL_SIL = 0.6             # s of trailing silence the emission anchor removes
HEAD_ROOM = 1.2            # s of headroom prepended to every window
MIN_SPEECH = 0.3           # s; shorter segments dropped
MAX_SEG = 18.0             # s; matches sherpa_live max_speech_duration
WIN = 512

def vad_windows(x, sr=16000):
    """Index-based state machine mirroring sherpa_live: Detected()-style
    speech flag, 0.6 s silence confirmation before closing a segment,
    emission-anchored window [seg_start - HEAD, last_speech_end + TAIL)."""
    sess = ort.InferenceSession(VAD, providers=["CPUExecutionProvider"])
    state = np.zeros((2, 1, 128), dtype=np.float32)
    sr_t = np.array(sr, dtype=np.int64)
    TAIL = int(TAIL_SIL * sr); HEAD = int(HEAD_ROOM * sr)
    wins, in_speech = [], False
    seg_start = last_speech_end = 0
    total = 0
    for i in range(0, len(x) - WIN, WIN):
        chunk = x[i:i + WIN].astype(np.float32)
        p, state = sess.run(None, {"input": chunk.reshape(1, WIN),
                                   "state": state, "sr": sr_t})
        speech = float(p[0][0]) > THRESHOLD
        total += WIN
        if speech:
            if not in_speech:
                in_speech = True
                seg_start = max(0, total - HEAD)
            last_speech_end = total
        elif in_speech and total - last_speech_end >= TAIL:
            # close: emission point == now; anchor everything to indices
            end = min(last_speech_end + TAIL, len(x))
            if end - seg_start >= MIN_SPEECH * sr:
                wins.append(x[seg_start:end])
            in_speech = False
        if in_speech and total - seg_start >= MAX_SEG * sr:
            end = min(total, len(x))
            wins.append(x[seg_start:end])
            in_speech = False
    if in_speech:
        end = min(last_speech_end + TAIL, len(x))
        if end - seg_start >= MIN_SPEECH * sr:
            wins.append(x[seg_start:end])
    return wins

def main():
    wav, = sys.argv[1:2]
    w = __import__("wave").open(wav)
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
    w.close()
    wins = vad_windows(x)
    phrases = [l.strip() for l in open(os.path.join(os.path.dirname(__file__), "phrases.list")) if l.strip()]
    os.makedirs("windows", exist_ok=True)
    for k, s in enumerate(wins, 1):
        ww = __import__("wave").open(f"windows/w{k:02d}.wav", "wb")
        ww.setnchannels(1); ww.setsampwidth(2); ww.setframerate(16000)
        ww.writeframes((np.clip(s, -1, 1) * 32767).astype(np.int16).tobytes())
        ww.close()
    print(f"{len(wins)} windows (expect ~{len(phrases)}); decode windows/ then "
          f"compare each transcript with phrases.list in order")

if __name__ == "__main__":
    main()
