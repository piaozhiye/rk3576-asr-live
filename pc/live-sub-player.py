#!/usr/bin/env python3
"""live-sub-player.py -- play a video with live ASR subtitles burned into it.

PC twin of the board's play-sub + sub-overlay pair, merged into one window:
  playbin (h264/aac) -> video -> textoverlay(live text) -> window
                      -> audio -> alsasink device=asrtap_auto
                                    (audible on the speakers AND tapped
                                     into /dev/shm/asrtap_ring for the
                                     recogniser -- the same chain the board
                                     runs, minus the NPU)

Subtitle lines arrive on a FIFO (SHERPA_SUBTITLE_FIFO of sherpa_live, by
default /tmp/asr_sub): growing drafts while the sentence is being spoken,
then the corrected+punctuated line replacing them.  Each line becomes the
overlay's text immediately -- that is the whole player.

usage: live-sub-player.py <media> [fifo] [stop_after_seconds]
"""
import os
import sys
import threading

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GLib", "2.0")
from gi.repository import GLib, Gst  # noqa: E402

FONT = "Noto Sans CJK SC, 36"


def die(msg):
    print("live-sub-player:", msg, file=sys.stderr, flush=True)
    sys.exit(1)


def main():
    if len(sys.argv) < 2:
        die(f"usage: {sys.argv[0]} <media> [fifo] [stop_after_seconds]")
    media = os.path.abspath(sys.argv[1])
    fifo = sys.argv[2] if len(sys.argv) > 2 else "/tmp/asr_sub"
    stop_after = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    if not os.path.exists(media):
        die(f"no such media: {media}")

    Gst.init(None)

    # Prefer VAAPI (the AMD iGPU decodes h264) over software avdec_h264:
    # with software decode the video thread starved the recogniser's capture
    # queue (75 dropped 32 ms chunks in 40 s = 2.4 s of holes punched into
    # the audio timeline, and the streaming model then decoded from wrong
    # positions -- stuttered, duplicated, hallucinated text).  Bumping the
    # rank makes playbin's decodebin pick the hardware decoder.
    for feature in ("vaapih264dec",):
        f = Gst.Registry.get().lookup_feature(feature)
        if f:
            f.set_rank(Gst.Rank.PRIMARY + 2)

    # video sink = textoverlay ! videoconvert ! autovideosink, wrapped in a
    # bin so playbin takes it as a whole and we keep a handle on the overlay.
    overlay = Gst.ElementFactory.make("textoverlay", "sub")
    if not overlay:
        die("no textoverlay (gstreamer1.0-plugins-base missing?)")
    overlay.set_property("text", "")

    # Font size must follow the VIDEO size, not the screen: at a fixed 36 px
    # a 960x544 lecture wrapped to 3-4 lines and covered most of the picture.
    # Movie-subtitle scale: ~1/30 of the video height (544p -> 18 px), thin
    # enough that a long sentence stays on one line near the bottom edge.
    def apply_font(height):
        size = max(14, height // 30)
        overlay.set_property("font-desc", f"Noto Sans CJK SC, {size}")

    apply_font(720)   # sane default until caps arrive
    overlay.set_property("valignment", "bottom")
    overlay.set_property("halignment", "center")
    overlay.set_property("line-alignment", "center")
    # Movie style: white text with the default black outline, NO shaded box
    # (the box was shading-value 80 grey under every line).
    conv = Gst.ElementFactory.make("videoconvert")
    vsink = Gst.ElementFactory.make("autovideosink")
    vbin = Gst.Bin.new("subbed_video")
    for el in (overlay, conv, vsink):
        vbin.add(el)
    overlay.link(conv)
    conv.link(vsink)
    # textoverlay's sink pad is named "video_sink", not "sink".
    vbin.add_pad(Gst.GhostPad.new("sink", overlay.get_static_pad("video_sink")))
    vbin_pad = vbin.get_static_pad("sink")

    def on_caps(_pad, _pspec):
        caps = vbin_pad.get_current_caps()
        if caps and caps.get_size() > 0:
            h = caps.get_structure(0).get_value("height")
            if isinstance(h, int) and h > 0:
                apply_font(h)

    vbin_pad.connect("notify::caps", on_caps)

    # Audio through the tap: audible on the speakers, downmixed copy into
    # the shm ring for the recogniser.
    asink = Gst.ElementFactory.make("alsasink")
    asink.set_property("device", "asrtap_auto")

    playbin = Gst.ElementFactory.make("playbin")
    playbin.set_property("uri", "file://" + media)
    playbin.set_property("video-sink", vbin)
    playbin.set_property("audio-sink", asink)
    playbin.set_state(Gst.State.PLAYING)

    loop = GLib.MainLoop()
    bus = playbin.get_bus()
    bus.add_signal_watch()

    def quit_loop(*_a):
        loop.quit()
        return False

    bus.connect("message::eos", lambda *a: quit_loop())
    bus.connect("message::error", lambda b, m: die(f"gst error: {m.parse_error()}"))
    if stop_after > 0:
        GLib.timeout_add_seconds(stop_after, quit_loop)

    state = {"n": 0}
    # Tee every received line here: a FIFO must have exactly ONE reader
    # (this player), so an extra `cat /tmp/asr_sub` logger would STEAL lines
    # from the screen (POSIX FIFO writes go to one reader, no broadcast) --
    # measured as the screen freezing on one sentence while drafts flew by.
    sub_log = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "subtitles.log"), "a", encoding="utf-8")

    def set_text(line):
        overlay.set_property("text", line)
        state["n"] += 1
        print(f"sub #{state['n']}: {line}", flush=True)
        print(line, file=sub_log, flush=True)
        return False

    def reader():
        # Blocking open is fine in this thread: the recogniser holds the
        # write end (or will reopen it).  Reopen on EOF to survive a
        # recogniser restart.
        while True:
            try:
                with open(fifo, "r") as f:
                    for line in f:
                        line = line.rstrip("\n")
                        if line:
                            GLib.idle_add(set_text, line)
            except OSError as e:
                print(f"fifo {fifo}: {e}; retrying", file=sys.stderr, flush=True)
                import time
                time.sleep(1.0)

    threading.Thread(target=reader, daemon=True).start()

    print(f"playing {media} (audio: asrtap_auto, subs: {fifo})", flush=True)
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    playbin.set_state(Gst.State.NULL)
    print(f"stopped after {state['n']} subtitle update(s)", flush=True)


if __name__ == "__main__":
    main()
