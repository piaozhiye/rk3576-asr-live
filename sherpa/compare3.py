#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 piaozhiye <piaozhiye@gmail.com>
"""Three-way ASR comparison on one recording, so the numbers are comparable.

Runs each available backend over the same WAV and scores it against the same
ground truth.  The point is that every backend sees identical audio, identical
VAD segmentation and identical scoring, so a difference in the table is a
difference in the recognizer and not in the harness.

Two scoring modes, because they answer different questions:

  offline  each phrase is cut from the source and decoded whole.  No VAD, no
           streaming, no segmentation.  This is the model's own accuracy.
  live     the recording is played through a speaker into the USB microphone and
           the board's own capture/VAD/segment path runs.  This is what a user
           experiences, and it is where segmentation and latency show up.

A backend that is not deployed is reported as absent rather than skipped in
silence, so a missing row can never be mistaken for a passing one.

usage:
    compare3.py --ground-truth <file> [--live-log <file> ...] [--only NAME]
"""

from __future__ import annotations

import argparse
import json
import re
import statistics as st
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

# Ground truth for the 10-phrase list, in playback order.  Kept here rather than
# read from a file so that a test run cannot silently score against the wrong
# list: the order is what the matching below depends on.
PHRASES = [
    "现在几点", "北京时间", "今天天气不错", "打开客厅的灯", "一二三四五",
    "我要听音乐", "明天早上八点", "音量太大了", "请把窗帘关上", "谢谢再见",
]

# Traditional -> simplified, single characters only.  whisper emits traditional
# because the C pipeline prints the model's raw bytes with no conversion step, so
# without this a correct whisper transcript scores as wrong on glyphs alone.
# Deliberately NOT a general converter: word-level ambiguity is out of scope for
# a command-phrase score, and a general table would quietly paper over real
# recognition errors.
T2S = {
    "現": "现", "幾": "几", "點": "点", "時": "时", "間": "间", "來": "来",
    "後": "后", "會": "会", "過": "过", "們": "们", "這": "这", "個": "个",
    "說": "说", "沒": "没", "對": "对", "開": "开", "關": "关", "聲": "声",
    "請": "请", "謝": "谢", "見": "见", "聽": "听", "樂": "乐", "電": "电",
    "讓": "让", "給": "给", "頭": "头", "動": "动", "員": "员", "經": "经",
    "線": "线", "車": "车", "東": "东", "馬": "马", "風": "风", "長": "长",
    "門": "门", "問": "问", "聞": "闻", "語": "语", "讀": "读", "寫": "写",
    "錯": "错", "邊": "边", "聲": "声", "麼": "么", "幾": "几", "樂": "乐",
}


def normalise(text: str) -> str:
    """Fold traditional glyphs to simplified so glyph choice is not scored."""
    return "".join(T2S.get(ch, ch) for ch in text)


def score(segments: Sequence[str], phrases: Sequence[str]) -> Dict[str, object]:
    """Walk segments against phrases in order, allowing merged and short results.

    Matching has to be symmetric, and getting that wrong silently corrupts the
    whole table.  A recognizer that drops a character ("现在几" for "现在几点")
    emits a segment that is a *substring* of the phrase, not a superstring; a
    one-directional test calls that spurious, fails to advance the cursor, and
    then misaligns every later segment.  So each segment is tested both ways
    against the phrase at the cursor:

      exact   the segment equals the phrase
      merged  the segment spans this phrase and the next one or more
      short   the segment overlaps the phrase without equalling it -- a dropped
              or substituted character, which is the case worth counting
      spur    the segment matches nothing; the cursor deliberately does NOT
              advance, so one spurious segment cannot cascade into the rest

    A merged segment counts its phrases as recognised, because the text is right,
    but it is reported separately so a wrong segmentation boundary stays visible
    instead of being scored as a success.
    """
    exact = merged = short = 0
    dropped = 0          # phrases skipped while resynchronising
    spurs: List[str] = []
    cursor = 0
    detail: List[Tuple[str, str, str]] = []       # phrase, got, verdict

    for seg in segments:
        seg = seg.strip()
        if not seg:
            continue
        cand = normalise(seg)
        # How many consecutive phrases from the cursor does this segment cover?
        span = 0
        while cursor + span < len(phrases) and phrases[cursor + span] in cand:
            span += 1
        if span == 0:
            # Not a superstring.  It may still be a truncated version of the
            # phrase we are waiting for, which is a real result, not a spurious
            # one -- so test that direction before giving up on the segment.
            want = phrases[cursor] if cursor < len(phrases) else ""
            if want and cand and cand in want:
                short += 1
                detail.append((want, seg, "short"))
                cursor += 1
                continue
            # Genuinely out of order, or the stream dropped a phrase earlier.
            # Resynchronise by locating this segment later in the phrase list.
            # Everything skipped on the way is a real miss and is counted as
            # one -- otherwise a single drop leaves the cursor stuck and turns
            # every later correct segment into a phantom false trigger.  The
            # cursor is moved and then FALLS THROUGH to the normal scoring below,
            # so the phrase this segment actually matched is still scored; an
            # earlier version continued past it and reported 2/78.
            target = next((i for i in range(cursor + 1, len(phrases))
                           if phrases[i] in cand), None)
            if target is None:
                spurs.append(seg)
                detail.append(("-", seg, "spurious"))
                continue
            dropped += target - cursor
            detail.append((phrases[cursor], "-", "missed"))
            cursor = target
            span = 0
            while cursor + span < len(phrases) and phrases[cursor + span] in cand:
                span += 1
        if span >= 2:
            merged += 1
            for k in range(span):
                detail.append((phrases[cursor + k], seg, "merged"))
        else:
            if cand == phrases[cursor]:
                exact += 1
                detail.append((phrases[cursor], seg, "exact"))
            else:
                short += 1
                detail.append((phrases[cursor], seg, "short"))
        cursor += span

    missed = dropped + max(0, len(phrases) - cursor)
    return {
        "segments": len([s for s in segments if s.strip()]),
        "phrases": len(phrases),
        "verbatim": exact,
        "merged": merged,
        "short": short,
        "missed": missed,
        "false_triggers": len(spurs),
        "detail": detail,
        "false_trigger_texts": spurs,
    }


def latency_from_log(text: str) -> Optional[Dict[str, float]]:
    """Pull the timing fields out of a sherpa_live log.

    Requires the t=/win=/lag= fields added with the capture/decode split; a log
    without them yields None rather than a guess, because latency measured from
    a log that never recorded when the text appeared is not a latency.
    """
    rows = re.findall(
        r"t=\s*([\d.]+)s win=\s*([\d.]+)-\s*([\d.]+) \([\d.]+s speech\) "
        r"lag=([+-][\d.]+)s q=(\d+) -> (.*?)\s+\(decode (\d+) ms\)", text)
    if not rows:
        return None
    lat = [float(t) - float(b) for t, _a, b, _l, _q, _x, _d in rows]
    lag = [float(l) for _t, _a, _b, l, _q, _x, _d in rows]
    dec = [float(d) for _t, _a, _b, _l, _q, _x, d in rows]
    q = [int(qq) for _t, _a, _b, _l, qq, _x, _d in rows]
    return {
        "latency_median_s": round(st.median(lat), 3),
        "latency_min_s": round(min(lat), 3),
        "latency_max_s": round(max(lat), 3),
        "latency_sigma_s": round(st.pstdev(lat), 3),
        "lag_median_s": round(st.median(lag), 3),
        "lag_min_s": round(min(lag), 3),
        "lag_max_s": round(max(lag), 3),
        "decode_median_ms": round(st.median(dec)),
        "queue_max": max(q),
        "n": len(rows),
    }


def live_from_log(path: Path, loops_hint: Optional[int] = None) -> Dict[str, object]:
    text = path.read_text(errors="replace")
    segments = re.findall(r"-> (.*?)\s+\(decode \d+ ms\)", text)
    if not segments:
        # whisper's pipeline prints one line per segment with no decode timing
        segments = re.findall(r"^whisper_text=(.*)$", text, re.M)
    n = len(segments)
    # loops must be told, not guessed: a run that ends mid-loop, or one that
    # merged two phrases into a single segment, both make segments/10 wrong, and
    # a wrong loop count silently changes the denominator of every rate below.
    # A capture window that expires mid-loop plays a fraction of the last pass,
    # so loops is a float: rounding it either invents phrases that were never
    # spoken (inflating the denominator) or drops ones that were.
    loops = loops_hint if loops_hint else (max(1.0, n / len(PHRASES)) if n else 0.0)
    whole = int(loops)
    phrases = PHRASES * whole + PHRASES[:int(round((loops - whole) * len(PHRASES)))]
    out: Dict[str, object] = {"log": str(path), "loops": loops}
    out.update(score(segments, phrases))
    out["latency"] = latency_from_log(text)
    return out


def offline_from_json(path: Path) -> Dict[str, object]:
    """Read sherpa-onnx-offline's JSON lines (one per wav) plus its RTF line."""
    text = path.read_text(errors="replace")
    texts = [json.loads(l)["text"] for l in text.splitlines()
             if l.strip().startswith('{"lang"')]
    rtf = None
    m = re.search(r"Real time factor \(RTF\): ([\d.]+) / ([\d.]+) = ([\d.]+)", text)
    if m:
        rtf = float(m.group(3))
    out: Dict[str, object] = {"log": str(path), "rtf": rtf}
    out.update(score(texts, PHRASES[:len(texts)]))
    return out


def render(name: str, r: Dict[str, object]) -> str:
    lat = r.get("latency")
    lines = [f"--- {name} ---"]
    if r.get("rtf") is not None:
        lines.append(f"  offline RTF        {r['rtf']}")
    if r.get("loops"):
        lines.append(f"  loops played       {r['loops']}")
    lines.append(
        f"  verbatim {r['verbatim']}/{r['phrases']}   merged {r['merged']}   "
        f"short {r['short']}   missed {r['missed']}   "
        f"false-triggers {r['false_triggers']}")
    if lat:
        lines.append(
            f"  latency  median {lat['latency_median_s']}s  "
            f"range {lat['latency_min_s']}-{lat['latency_max_s']}s  "
            f"sigma {lat['latency_sigma_s']}s")
        lines.append(
            f"  lag      median {lat['lag_median_s']}s  "
            f"range {lat['lag_min_s']}-{lat['lag_max_s']}s   "
            f"decode median {lat['decode_median_ms']}ms   "
            f"queue max {lat['queue_max']}")
    else:
        lines.append("  latency  not measurable from this log (no t=/win= fields)")
    return "\n".join(lines)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--live-log", action="append", default=[],
                    help="path to a sherpa_live log; name it NAME=path")
    ap.add_argument("--offline-log", action="append", default=[],
                    help="path to a sherpa-onnx-offline log with JSON lines")
    ap.add_argument("--only", help="score just this backend")
    ap.add_argument("--loops", type=float,
                    help="how many times the phrase list was played; "
                         "defaults to a guess from the segment count")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of text")
    args = ap.parse_args(argv)

    if not args.live_log and not args.offline_log:
        ap.error("give at least one --live-log or --offline-log")

    def split(spec: str) -> Tuple[str, str, Optional[float]]:
        """NAME=PATH[@LOOPS].

        Loops has to be per backend: each run played a different number of
        passes, and a single global value silently reuses one run's ground-truth
        length for another's, which turns that run's surplus segments into
        phantom false triggers.
        """
        name, _, rest = spec.partition("=")
        path, _, loops = rest.partition("@")
        return name, path, (float(loops) if loops else None)

    report: Dict[str, object] = {}
    for spec in args.live_log:
        name, path, loops = split(spec)
        if args.only and args.only != name:
            continue
        p = Path(path)
        if not p.exists():
            report[name] = {"absent": f"log not found: {path}"}
            continue
        report[name] = live_from_log(p, loops if loops is not None else args.loops)
    for spec in args.offline_log:
        name, _, path = spec.partition("=")
        if args.only and args.only != name:
            continue
        p = Path(path)
        if not p.exists():
            report[name] = {"absent": f"log not found: {path}"}
            continue
        report[name] = offline_from_json(p)

    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 0
    for name, r in report.items():
        if "absent" in r:
            print(f"--- {name} ---\n  ABSENT: {r['absent']}")
        else:
            print(render(name, r))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
