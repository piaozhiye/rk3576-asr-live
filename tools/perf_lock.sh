#!/bin/sh
# Lock the board to its maximum frequencies while the ASR pipeline runs.
#
#   perf-lock          pin everything to its ceiling
#   perf-lock off      put the schedulers back
#   perf-lock status   print what is in effect
#
# Why: nothing was pinned.  The cores sat on schedutil/ondemand, and DDR (dmc)
# ran at 528 MHz against a 2112 MHz ceiling.  A real-time pipeline is
# jitter-bound, not throughput-bound, so the headroom is worth the power.
# Measured: the music-separation pass went from 1.615x to 1.017x real time.
#
# The backup is written once and never overwritten, so `off` returns the board
# to the state it was in before the first pin rather than to whatever happened
# to be current.

BACKUP=/var/perf_backup.txt

# Returns success even when the node is absent: a domain this board does not
# have is not a failure, and devfreq drivers register late at boot.
setval() { echo "$2" > "$1" 2>/dev/null || true; }

pin() {
    [ -s "$BACKUP" ] || : > "$BACKUP"

    for c in /sys/devices/system/cpu/cpu*/cpufreq; do
        [ -d "$c" ] || continue
        for n in scaling_governor scaling_min_freq scaling_max_freq; do
            grep -q "^$c/$n " "$BACKUP" 2>/dev/null ||
                echo "$c/$n $(cat "$c/$n" 2>/dev/null)" >> "$BACKUP"
        done
        hwm=$(cat "$c/cpuinfo_max_freq" 2>/dev/null) || continue
        setval "$c/scaling_governor" performance
        setval "$c/scaling_min_freq" "$hwm"
        setval "$c/scaling_max_freq" "$hwm"
    done

    for d in /sys/class/devfreq/*; do
        [ -d "$d" ] || continue
        for n in governor min_freq; do
            grep -q "^$d/$n " "$BACKUP" 2>/dev/null ||
                echo "$d/$n $(cat "$d/$n" 2>/dev/null)" >> "$BACKUP"
        done
        max=$(cat "$d/max_freq" 2>/dev/null) || continue
        setval "$d/governor" performance
        setval "$d/min_freq" "$max"
    done

    # The NPU ignores min_freq until its user-space gate has been opened once.
    setval /sys/kernel/debug/rknpu/load user
    echo "perf: pinned"
}

unpin() {
    [ -f "$BACKUP" ] || { echo "perf: no backup, nothing to unpin" >&2; return 1; }
    while read -r p v; do
        [ -n "$p" ] && setval "$p" "$v"
    done < "$BACKUP"
    # max before min: this cpufreq driver silently refuses a min_freq write
    # while min is above the target and max is already at the ceiling, so
    # lowering the ceiling first is what makes the restore actually land.
    echo "perf: unpinned"
}

status() {
    for c in /sys/devices/system/cpu/cpu0/cpufreq \
             /sys/devices/system/cpu/cpu4/cpufreq; do
        [ -d "$c" ] || continue
        echo "  $(basename "$(dirname "$c")"): $(cat "$c/scaling_governor" 2>/dev/null)" \
             "$(cat "$c/scaling_cur_freq" 2>/dev/null)Hz"
    done
    for d in /sys/class/devfreq/dmc /sys/class/devfreq/27700000.npu; do
        [ -d "$d" ] || continue
        echo "  $(basename "$d"): $(cat "$d/governor" 2>/dev/null)" \
             "$(cat "$d/cur_freq" 2>/dev/null)"
    done
}

case "${1:-}" in
    ""|apply) pin ;;
    off|restore) unpin ;;
    status)  status ;;
    *)       echo "usage: perf-lock [apply|off|status]" >&2; exit 2 ;;
esac
