#!/bin/sh
# Play a media file with live subtitles, on the board, over the ASR tap.
#
#   usage: play_sub.sh <file-or-uri> [seconds]
#
# Assumes the streaming model is installed (tools/install_stream_model.sh) and
# that sherpa_live is in /data/rknn-test/sherpa.  Run the recogniser first, in
# another terminal or with & -- the tap drops everything until a reader is
# attached, which is silent and easy to misread as "the tap is broken".
#
# Two things in here are not obvious and both were found by measurement:
#
#  * pixel-aspect-ratio=1/1.  Videos that are not square-pixel -- mp2.mp4 is
#    960x544 with PAR 136/135 -- make waylandsink ask weston for a viewport
#    968 pixels wide against a 960-pixel buffer, and weston refuses the whole
#    surface with "wp_viewport: viewport source outside buffer".  The video
#    plays, the clock runs, the audio taps, and the screen stays black.
#    Forcing square pixels lets videoscale absorb the ratio difference, so the
#    surface size matches the buffer.  720p video (PAR 1/1) is unaffected and
#    this filter is harmless there.
#
#  * XDG_RUNTIME_DIR and WAYLAND_DISPLAY.  Over adb shell neither is set, so
#    waylandsink would fall back to something that cannot display.  The socket
#    is /run/wayland-0 on this board.
#
# Audio goes to asrtap_auto, which is the ALSA plugin that mirrors playback
# into the recogniser's FIFO at 16 kHz mono.  It does not replace the real
# output; you still hear the movie.
#
# Subtitle window placement (measured on weston 14, xdg-shell v3 tiling):
# weston tiles toplevels with the most recently mapped one FIRST, so the
# subtitle overlay must be (re)started AFTER the player starts -- this script
# does that below, which pins the band to the top of the screen with the
# video underneath.  There is no protocol way to pin a toplevel: this weston
# has xdg-shell v3 (no set_positioner), and layer-shell is not supported.

DIR=/data/rknn-test/sherpa

URI="$1"
SECS="${2:-0}"
[ -n "$URI" ] || { echo "usage: $0 <file-or-uri> [seconds]" >&2; exit 2; }
case "$URI" in file://*) ;; /*) URI="file://$URI" ;; esac

export XDG_RUNTIME_DIR=/run
export WAYLAND_DISPLAY=wayland-0

echo "recogniser first (or tools/supervisor.sh start), then this script."
echo "audio goes to asrtap_auto; the recogniser reads it back from the"
echo "asr_capture ALSA node (shm ring -- no FIFO on this path anymore)."
echo
echo "now playing $URI"

# async=false on BOTH sinks: playbin's PAUSED->PLAYING transition waits for
# every sink's preroll -- on a cold mpp video decoder that is ~7.5 s, and
# audio only sends a 0.3 s preroll burst then stalls until PLAYING.  async
# .false lets the pipeline go PLAYING without waiting; video keeps sync=true
# so playback itself is still clock-paced.  Measured cold: first subtitle
# 8.28 s -> 0.53 s (2026-09-30).
VSINK="videoconvert ! videoscale ! video/x-raw,format=NV12,pixel-aspect-ratio=1/1 ! waylandsink async=false "
# sync=false on the audio sink: playbin's audio-video preroll sync held the
# first audio frames back until the video pipeline aligned on the clock --
# measured 2026-09-30, ~6 s on mp4 (first subtitle 8.0 s wall) vs 0.5 s with
# sync off.  The board has no speaker: the ONLY consumer of this audio is
# the ASR tap, so audio running off-clock costs nothing audibly while video
# keeps its own sync.
ASINK="alsasink device=asrtap_auto sync=false async=false"

# Player first, then the overlay: last-mapped toplevel tiles first (top).
(
  if [ "$SECS" -gt 0 ] 2>/dev/null; then
      timeout "$SECS" gst-launch-1.0 -q playbin uri="$URI" \
          video-sink="$VSINK" audio-sink="$ASINK"
  else
      gst-launch-1.0 -q playbin uri="$URI" \
          video-sink="$VSINK" audio-sink="$ASINK"
  fi
) &
PLAYER_PID=$!
sleep 2

# Re-map the subtitle band so it tiles ahead of the freshly started player.
FONT=/usr/share/fonts/source-han-sans-cn/SourceHanSansCN-Regular.otf
pkill -x sub-overlay 2>/dev/null
rm -f /tmp/asr_sub 2>/dev/null
cd "$DIR" || exit 1
nohup ./sub-overlay /tmp/asr_sub "$FONT" >> /tmp/so.log 2>&1 &
sleep 1

wait "$PLAYER_PID"
PLAYER_STATUS=$?
# A requested duration ends gst-launch via timeout(1), whose 124 status is
# expected.  Other playback errors must still reach the caller.
if [ "$SECS" -gt 0 ] 2>/dev/null && [ "$PLAYER_STATUS" -eq 124 ]; then
    exit 0
fi
exit "$PLAYER_STATUS"
