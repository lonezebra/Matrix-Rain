#!/bin/zsh
# Regression test for the "chugs after a few seconds" bug: runs the real
# MatrixRainView (via the PreviewHost harness in this directory, not the
# legacy ScreenSaverEngine binary -- that has moved/disappeared across
# recent macOS releases) and samples process CPU%, achieved framerate, and
# system GPU active residency over time. FAILS if late-run CPU% is
# significantly higher than early-run CPU% -- that growth pattern is
# exactly the symptom the dirty-rect invalidation fix (see
# ../Sources/MatrixRainView.swift) was written to kill.
#
# CPU% comes from `ps` (no privileges needed). GPU active residency comes
# from `powermetrics`, which requires sudo; if you decline/can't provide
# it, the script still runs and reports GPU as n/a -- CPU and fps are the
# primary regression signal, GPU is supplementary.
#
# Usage: measure_utilization.sh [duration_sec] [width] [height]
#   duration_sec  total sample window (default 60)
#   width height  preview window size (default 1920 1080)

set -u
SCRIPT_DIR="$(cd "$(dirname "${(%):-%x}")" && pwd)"
DURATION="${1:-60}"
WIDTH="${2:-1920}"
HEIGHT="${3:-1080}"
SAMPLE_INTERVAL=2
REGRESSION_THRESHOLD_PCT=25

# Always (re)build via `make` before measuring: `make`'s own dependency
# tracking rebuilds the bundle iff Sources/*.swift changed since the last
# build, and is a fast no-op otherwise. This exists because an earlier
# version of this script used whatever bundle happened to already be on
# disk -- which silently benchmarked a stale build (from before a perf fix
# landed) and produced a false "still chugging" regression report.
echo "note: building MatrixRain.saver (make is a no-op if already current)"
if ! make -C "$SCRIPT_DIR/.." >/dev/null; then
    echo "error: 'make' failed in macos/ -- see output above" >&2
    exit 2
fi
SAVER="$SCRIPT_DIR/../MatrixRain.saver"
if [ ! -d "$SAVER" ]; then
    echo "error: MatrixRain.saver not found after 'make' -- unexpected" >&2
    exit 2
fi
echo "note: using freshly built bundle at $SAVER"

HOST_SRC="$SCRIPT_DIR/PreviewHost.swift"
HOST_BIN="$SCRIPT_DIR/PreviewHost"
if [ ! -x "$HOST_BIN" ] || [ "$HOST_SRC" -nt "$HOST_BIN" ]; then
    echo "building PreviewHost benchmark harness (one-time)..."
    if ! swiftc -O "$HOST_SRC" -o "$HOST_BIN" -framework AppKit -framework ScreenSaver; then
        echo "error: failed to build $HOST_SRC" >&2
        exit 2
    fi
fi

GPU_AVAILABLE=1
if ! sudo -v; then
    GPU_AVAILABLE=0
    echo "note: no sudo access -- GPU column will read n/a (powermetrics needs root)"
fi

FPS_LOG=$(mktemp)
PM_OUT=$(mktemp)
cleanup() {
    [ -n "${HOST_PID:-}" ] && kill "$HOST_PID" 2>/dev/null
    [ -n "${PM_PID:-}" ] && sudo kill "$PM_PID" 2>/dev/null
    rm -f "$FPS_LOG" "$PM_OUT"
}
trap cleanup EXIT INT TERM

echo "launching MatrixRain preview host (${WIDTH}x${HEIGHT}) for ${DURATION}s"
MATRIX_RAIN_FPSLOG=1 "$HOST_BIN" "$SAVER" "$WIDTH" "$HEIGHT" "$((DURATION + 3))" >"$FPS_LOG" 2>&1 &
HOST_PID=$!
sleep 1
if ! kill -0 "$HOST_PID" 2>/dev/null; then
    echo "error: PreviewHost exited immediately -- output was:" >&2
    cat "$FPS_LOG" >&2
    exit 2
fi

if [ "$GPU_AVAILABLE" = 1 ]; then
    sudo powermetrics --samplers gpu_power -i "$((SAMPLE_INTERVAL * 1000))" \
        -n "$((DURATION / SAMPLE_INTERVAL))" > "$PM_OUT" 2>/dev/null &
    PM_PID=$!
fi

SAMPLES=$((DURATION / SAMPLE_INTERVAL))
declare -a CPU_PCT FPS_VAL TIMESTAMPS
fps_line=0

printf "%6s  %8s  %8s\n" "t(s)" "cpu%" "fps"
for ((i = 1; i <= SAMPLES; i++)); do
    sleep "$SAMPLE_INTERVAL"
    if ! kill -0 "$HOST_PID" 2>/dev/null; then
        echo "error: PreviewHost died during sampling (sample $i) -- output was:" >&2
        cat "$FPS_LOG" >&2
        exit 2
    fi

    cpu_pct=$(ps -o %cpu= -p "$HOST_PID" 2>/dev/null | tr -d ' ')
    [ -z "$cpu_pct" ] && cpu_pct="n/a"

    total_lines=$(wc -l < "$FPS_LOG" 2>/dev/null | tr -d ' ')
    [ -z "$total_lines" ] && total_lines=0
    fps=""
    if [ "$total_lines" -gt "$fps_line" ]; then
        fps=$(sed -n "$((fps_line + 1)),${total_lines}p" "$FPS_LOG" |
              grep -oE 'fps=[0-9.]+' | cut -d= -f2 |
              awk '{s+=$1; n++} END {if (n>0) printf "%.2f", s/n}')
        fps_line=$total_lines
    fi
    [ -z "$fps" ] && fps="n/a"

    CPU_PCT[i]=$cpu_pct
    FPS_VAL[i]=$fps
    TIMESTAMPS[i]=$((i * SAMPLE_INTERVAL))
    printf "%6s  %8s  %8s\n" "${TIMESTAMPS[i]}" "$cpu_pct" "$fps"
done

kill "$HOST_PID" 2>/dev/null
wait "$HOST_PID" 2>/dev/null
HOST_PID=""
if [ -n "${PM_PID:-}" ]; then
    wait "$PM_PID" 2>/dev/null
    PM_PID=""
fi

# GPU: report powermetrics' active-residency samples, if we got any.
gpu_vals=("${(@f)$(grep -E 'GPU HW active residency' "$PM_OUT" 2>/dev/null | grep -oE '[0-9.]+%' | tr -d '%')}")
if [ "${#gpu_vals[@]}" -gt 0 ]; then
    echo
    echo "GPU active residency samples: ${gpu_vals[*]}%"
else
    echo
    echo "GPU active residency: n/a (no sudo, or powermetrics produced no samples -- raw output at $PM_OUT)"
fi

THIRD=$((SAMPLES / 3))
if [ "$THIRD" -lt 1 ]; then
    echo "warning: duration too short for a meaningful early/late comparison (need >=${SAMPLE_INTERVAL}x3 s)"
    exit 0
fi

early_sum=0
for ((i = 1; i <= THIRD; i++)); do
    [ "${CPU_PCT[i]}" != "n/a" ] && early_sum=$(echo "$early_sum + ${CPU_PCT[i]}" | bc)
done
early_avg=$(echo "scale=2; $early_sum / $THIRD" | bc)

late_sum=0
for ((i = SAMPLES - THIRD + 1; i <= SAMPLES; i++)); do
    [ "${CPU_PCT[i]}" != "n/a" ] && late_sum=$(echo "$late_sum + ${CPU_PCT[i]}" | bc)
done
late_avg=$(echo "scale=2; $late_sum / $THIRD" | bc)

fps_sum=0
fps_n=0
for ((i = 1; i <= SAMPLES; i++)); do
    [ "${FPS_VAL[i]}" != "n/a" ] && { fps_sum=$(echo "$fps_sum + ${FPS_VAL[i]}" | bc); fps_n=$((fps_n + 1)); }
done
if [ "$fps_n" -gt 0 ]; then
    fps_avg=$(echo "scale=2; $fps_sum / $fps_n" | bc)
else
    fps_avg="n/a"
fi

echo
echo "early-window avg CPU%: $early_avg  (first ${THIRD} samples, screen still filling)"
echo "late-window  avg CPU%: $late_avg  (last ${THIRD} samples, screen steady-state)"
echo "avg fps: $fps_avg   (target: 30)"

if [ "$early_avg" = "0" ] || [ -z "$early_avg" ]; then
    echo "note: early-window CPU% was 0 -- skipping growth ratio (would divide by zero)"
    exit 0
fi

growth=$(echo "scale=2; 100 * ($late_avg - $early_avg) / $early_avg" | bc 2>/dev/null)
echo "growth: ${growth}%  (fail threshold: +${REGRESSION_THRESHOLD_PCT}%)"

if (( $(echo "$growth > $REGRESSION_THRESHOLD_PCT" | bc -l) )); then
    echo "FAIL: CPU usage grew as the screen filled -- this is the chugging regression"
    exit 1
fi
echo "PASS: CPU usage stayed flat as the screen filled"
exit 0
