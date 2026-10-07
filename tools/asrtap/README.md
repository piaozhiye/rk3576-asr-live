# asrtap -- ALSA playback-tap + capture-node plugins (prebuilt)

The video-subtitle chain's audio transport: a playback-side extplug tees
audio into a lock-free POSIX-shm ring (16 kHz mono), and a capture-side
ioplug exposes it as an ordinary ALSA capture device.  No FIFOs, no
lifecycle coupling: the player and the recogniser restart independently.

```
player → asrtap_auto (plug, 48 k) → asrtap (extplug: passthrough + downmix)
        → shm ring /asrtap_ring (64 k frames, overwrite-oldest)
        → asr_capture (ioplug; start_backlog 1) → sherpa_live
```

**This directory ships prebuilt binaries only** -- the plugin source is
not part of this repository.  Each arch directory holds the same two
modules, dynamically linked against plain `libasound.so.2` + libc:

| 目录 | 架构 | 用途 |
|---|---|---|
| `aarch64/` | armv8 64-bit | RK3576 板端(`install.sh` 部署这两个) |
| `armhf/`   | armv7 32-bit | 32 位 buildroot 系统 |
| `x86_64/`  | x86-64 | PC 链路(`pc/setup-pc.sh` 接线) |

## Install on the board

    ./install.sh

Pushes the `aarch64/` pair to `/usr/lib/alsa-lib/` and appends the three
pcm entries to `/etc/asound.conf` (idempotent, backup kept).  This board
is 64-bit only -- `armhf/` exists for other targets.

On the PC the chain uses the x86_64 prebuilts; `pc/setup-pc.sh` points
ALSA's `pcm_type.*.lib` at them inside a self-contained `pc/alsa.conf`.

Verify:

    adb shell 'aplay -L | grep -E "asrtap|asr_capture"'
    adb shell 'gst-launch-1.0 playbin uri=file://... audio-sink="alsasink device=asrtap_auto"'  # then
    adb shell 'arecord -D asr_capture -d 3 /tmp/t.wav'   # captures the tap

## Files

- `aarch64|armhf|x86_64/libasound_module_pcm_asrtap.so`
                   playback extplug (tee + 48k→16k downmix; slave pinned
                   stereo -- a mono slave loses hardware pacing, measured
                   ~2.2x real time racing)
- `aarch64|armhf|x86_64/libasound_module_pcm_asrtap_c.so`
                   capture ioplug (start_backlog session replay; pump steps
                   clamped to buffer/2; 10 ms poll timer; verbose logging
                   behind ASRTAP_C_VERBOSE)
- `asound.conf.asrtap` / `install.sh`   board deploy + pcm entries
- the shm-ring layout (wr/rd/dropped + wr_us freshness + sess_wr session
  start) is pinned by the binaries on both sides; they must come from
  the same build generation

## Hard-won semantics (do not "simplify" without re-measuring)

- extplug transfer must not block; slave pinned to stereo (mono pacing bug).
- ioplug: the ring pull lives in `.pointer` AND `.transfer`; ACCESS/FORMAT
  are list-type params on this alsa-lib (minmax fails); overriding
  poll_descriptors BLOCKS snd_pcm_wait forever -- only poll_revents is safe.
- Reader after idle: needs a close/reopen cycle to resume delivery (the
  recogniser's 0.5 s watchdog does this); `start_backlog 1` catches unread
  audio from the current writer session without replaying frames already
  consumed before a same-ring reconnect.
- ALSA capture buffer must be >= ring depth (65536 frames): bursts bigger
  than the buffer overrun the stream into a stop/recover death spiral.
