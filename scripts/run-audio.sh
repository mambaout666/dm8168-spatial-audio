#!/bin/sh
# One ALSA capture -> backend -> ALSA playback session; Linux only.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
backend=cpu
bank=
plugin=
capture=default
playback=default
duration=30
runtime=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --backend|--bank|--rpe-plugin|--capture-device|--playback-device|--duration|--runtime-dir)
            [ "$#" -ge 2 ] || { printf 'Missing value for %s\n' "$1" >&2; exit 2; }
            case "$1" in
                --backend) backend=$2;; --bank) bank=$2;; --rpe-plugin) plugin=$2;;
                --capture-device) capture=$2;; --playback-device) playback=$2;;
                --duration) duration=$2;; --runtime-dir) runtime=$2;;
            esac
            shift 2;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2;;
    esac
done
case "$duration" in ''|*[!0-9]*|0) printf 'Duration must be a positive number of seconds\n' >&2; exit 2;; esac
case "$backend" in cpu|rpe) ;; *) printf 'Backend must be cpu or rpe\n' >&2; exit 2;; esac
if [ "$backend" = rpe ] && [ -z "$plugin" ]; then printf 'RPE requires a real --rpe-plugin library\n' >&2; exit 2; fi
for executable in arecord aplay; do command -v "$executable" >/dev/null 2>&1 || { printf '%s is missing\n' "$executable" >&2; exit 2; }; done
[ -x "$project/build/spatial-audio" ] || { printf 'Build spatial-audio first\n' >&2; exit 2; }
if [ -z "$runtime" ]; then runtime=$(mktemp -d /tmp/dm8168-audio.XXXXXX); else mkdir -m 700 -p -- "$runtime"; fi
runtime=$(CDPATH= cd -- "$runtime" && pwd -P)
mkdir -- "$runtime/session.lock" || { printf 'Runtime directory has an active session lock\n' >&2; exit 2; }
mkdir -p -- "$project/results/board"
capture_pid= playback_pid= backend_pid=
capture_fifo_created=0
playback_fifo_created=0
cleanup() {
    trap - EXIT INT TERM
    for pid in "$capture_pid" "$backend_pid" "$playback_pid"; do
        if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || :; fi
    done
    for pid in "$capture_pid" "$backend_pid" "$playback_pid"; do
        if [ -n "$pid" ]; then wait "$pid" 2>/dev/null || :; fi
    done
    # Only remove the two audio FIFOs created below, never the runtime tree.
    if [ "$capture_fifo_created" = 1 ]; then rm -f -- "$runtime/capture.fifo"; fi
    if [ "$playback_fifo_created" = 1 ]; then rm -f -- "$runtime/playback.fifo"; fi
    rmdir -- "$runtime/session.lock" 2>/dev/null || :
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkfifo -m 600 -- "$runtime/capture.fifo"
capture_fifo_created=1
mkfifo -m 600 -- "$runtime/playback.fifo"
playback_fifo_created=1
set -- "$project/build/spatial-audio" --backend "$backend" --input "$runtime/capture.fifo" \
    --output "$runtime/playback.fifo" --runtime-dir "$runtime" --sample-rate 48000 \
    --stats "$project/results/board/blocks.csv"
if [ -n "$bank" ]; then set -- "$@" --bank "$bank"; else printf 'Using SYNTHETIC test filters, not measured HRIR.\n' >&2; fi
if [ -n "$plugin" ]; then set -- "$@" --rpe-plugin "$plugin"; fi
aplay -D "$playback" -t raw -f S16_LE -r 48000 -c 2 "$runtime/playback.fifo" \
    2>"$project/results/board/playback.log" & playback_pid=$!
"$@" 2>"$project/results/board/backend.log" & backend_pid=$!
arecord -D "$capture" -t raw -f S16_LE -r 48000 -c 2 -d "$duration" "$runtime/capture.fifo" \
    2>"$project/results/board/capture.log" & capture_pid=$!
printf 'Runtime directory: %s\n' "$runtime"
printf 'Qt client: ./build/qt/dm8168-audio-qt --runtime-dir "%s"\n' "$runtime"
printf 'Qt4 on the board additionally needs -qws.\n'
# Detect an early failure in any process instead of waiting forever on a FIFO.
while kill -0 "$capture_pid" 2>/dev/null && kill -0 "$backend_pid" 2>/dev/null && kill -0 "$playback_pid" 2>/dev/null; do sleep 1; done
if ! kill -0 "$capture_pid" 2>/dev/null; then
    wait "$capture_pid" || { printf 'Capture failed; inspect capture.log\n' >&2; exit 1; }
    capture_pid=
    # The finite recording closes its FIFO; backend and playback drain and exit.
    wait "$backend_pid" || { printf 'Backend failed; inspect backend.log\n' >&2; exit 1; }
    backend_pid=
    wait "$playback_pid" || { printf 'Playback failed; inspect playback.log\n' >&2; exit 1; }
    playback_pid=
else
    printf 'Backend or playback exited early; inspect results/board logs.\n' >&2
    exit 1
fi
printf 'Session completed. These are host/board-local measurements, not an end-to-end latency measurement.\n'
