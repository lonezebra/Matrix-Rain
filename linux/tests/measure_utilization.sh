#!/usr/bin/env bash
# Regression test for the "chugs after a few seconds" bug: runs the real
# (fps-capped, windowed) renderer for a while and samples process CPU%
# over time. FAILS if late-run CPU% is significantly higher than
# early-run CPU% -- that growth pattern is exactly the symptom the
# dirty-cell rendering fix (see ../matrix_rain.c) was written to kill.
#
# GPU utilization is sampled best-effort (nvidia-smi / intel_gpu_top /
# radeontop, whichever is present) for informational purposes only: the
# X11/Xft renderer is CPU-side rasterization, so "no GPU tool found" or
# "0% GPU" is an expected, passing result, not a failure.
#
# Usage: measure_utilization.sh [duration_sec] [width] [height]
#   duration_sec  total sample window (default 60)
#   width height  virtual display size (default 1920 1080)
#
# Requires: the binary built at ../matrix-rain (run `make` first).
# Uses Xvfb if no DISPLAY is already set; otherwise renders into a real
# X session so you can watch it while the numbers are collected.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$SCRIPT_DIR/../matrix-rain"
DURATION="${1:-60}"
WIDTH="${2:-1920}"
HEIGHT="${3:-1080}"
SAMPLE_INTERVAL=2
REGRESSION_THRESHOLD_PCT=25   # late window may exceed early window by at most this many %

if [ ! -x "$BIN" ]; then
    echo "error: $BIN not found or not executable -- run 'make' in linux/ first" >&2
    exit 2
fi

CLK_TCK=$(getconf CLK_TCK)

find_gpu_tool() {
    if command -v nvidia-smi >/dev/null 2>&1; then echo "nvidia-smi"
    elif command -v intel_gpu_top >/dev/null 2>&1; then echo "intel_gpu_top"
    elif command -v radeontop >/dev/null 2>&1; then echo "radeontop"
    else echo ""
    fi
}

sample_gpu_pct() {
    case "$1" in
        nvidia-smi)
            nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits 2>/dev/null | head -1
            ;;
        intel_gpu_top)
            # -s ms, -o - prints one JSON-ish sample to stdout; best-effort parse
            timeout 1 intel_gpu_top -s 500 -o - 2>/dev/null | grep -m1 -oE '"busy":[0-9.]+' | cut -d: -f2
            ;;
        radeontop)
            timeout 1 radeontop -d - -l 1 2>/dev/null | grep -oE 'gpu [0-9.]+%' | grep -oE '[0-9.]+'
            ;;
        *) echo "" ;;
    esac
}

read_proc_ticks() {
    # utime (14th field) + stime (15th field) from /proc/<pid>/stat
    awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null
}

cleanup() {
    [ -n "${RAIN_PID:-}" ] && kill "$RAIN_PID" 2>/dev/null
    [ -n "${XVFB_PID:-}" ] && kill "$XVFB_PID" 2>/dev/null
}
trap cleanup EXIT INT TERM

OWN_XVFB=0
if [ -z "${DISPLAY:-}" ]; then
    OWN_XVFB=1
    XVFB_DISPLAY=":$((90 + RANDOM % 900))"
    Xvfb "$XVFB_DISPLAY" -screen 0 "${WIDTH}x${HEIGHT}x24" >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 1
    export DISPLAY="$XVFB_DISPLAY"
fi

GPU_TOOL=$(find_gpu_tool)
if [ -z "$GPU_TOOL" ]; then
    echo "note: no GPU monitoring tool found (nvidia-smi/intel_gpu_top/radeontop) -- GPU column will read n/a"
else
    echo "note: sampling GPU utilization via $GPU_TOOL"
fi

echo "launching matrix-rain -window ${WIDTH}x${HEIGHT} for ${DURATION}s on display $DISPLAY"
"$BIN" -window -density 0.9 -speed 1.5 &
RAIN_PID=$!
sleep 1
if ! kill -0 "$RAIN_PID" 2>/dev/null; then
    echo "error: matrix-rain exited immediately -- check DISPLAY/X server" >&2
    exit 2
fi

SAMPLES=$((DURATION / SAMPLE_INTERVAL))
declare -a CPU_PCT GPU_PCT TIMESTAMPS
prev_ticks=$(read_proc_ticks "$RAIN_PID")
prev_time=$(date +%s.%N)

printf "%6s  %8s  %8s\n" "t(s)" "cpu%" "gpu%"
for ((i = 1; i <= SAMPLES; i++)); do
    sleep "$SAMPLE_INTERVAL"
    if ! kill -0 "$RAIN_PID" 2>/dev/null; then
        echo "error: matrix-rain died during sampling (frame $i)" >&2
        exit 2
    fi
    now_ticks=$(read_proc_ticks "$RAIN_PID")
    now_time=$(date +%s.%N)
    dt_ticks=$((now_ticks - prev_ticks))
    dt_time=$(echo "$now_time - $prev_time" | bc)
    cpu_pct=$(echo "scale=2; 100 * $dt_ticks / $CLK_TCK / $dt_time" | bc | sed 's/^\./0./')
    gpu_pct=$(sample_gpu_pct "$GPU_TOOL")
    [ -z "$gpu_pct" ] && gpu_pct="n/a"
    CPU_PCT[i]=$cpu_pct
    GPU_PCT[i]=$gpu_pct
    TIMESTAMPS[i]=$((i * SAMPLE_INTERVAL))
    printf "%6s  %8s  %8s\n" "${TIMESTAMPS[i]}" "$cpu_pct" "$gpu_pct"
    prev_ticks=$now_ticks
    prev_time=$now_time
done

kill "$RAIN_PID" 2>/dev/null
wait "$RAIN_PID" 2>/dev/null
RAIN_PID=""

# Compare average CPU% of the first third of samples vs the last third --
# this is the actual regression check for the "chugs after a few seconds" bug.
THIRD=$((SAMPLES / 3))
if [ "$THIRD" -lt 1 ]; then
    echo "warning: duration too short for a meaningful early/late comparison (need >=${SAMPLE_INTERVAL}x3 s)"
    exit 0
fi

early_sum=0
for ((i = 1; i <= THIRD; i++)); do early_sum=$(echo "$early_sum + ${CPU_PCT[i]}" | bc); done
early_avg=$(echo "scale=2; $early_sum / $THIRD" | bc)

late_sum=0
for ((i = SAMPLES - THIRD + 1; i <= SAMPLES; i++)); do late_sum=$(echo "$late_sum + ${CPU_PCT[i]}" | bc); done
late_avg=$(echo "scale=2; $late_sum / $THIRD" | bc)

echo
echo "early-window avg CPU%: $early_avg  (first ${THIRD} samples, screen still filling)"
echo "late-window  avg CPU%: $late_avg  (last ${THIRD} samples, screen steady-state)"

growth=$(echo "scale=2; 100 * ($late_avg - $early_avg) / $early_avg" | bc 2>/dev/null)
[ -z "$growth" ] && growth=0
echo "growth: ${growth}%  (fail threshold: +${REGRESSION_THRESHOLD_PCT}%)"

if (( $(echo "$growth > $REGRESSION_THRESHOLD_PCT" | bc -l) )); then
    echo "FAIL: CPU usage grew as the screen filled -- this is the chugging regression"
    exit 1
fi
echo "PASS: CPU usage stayed flat as the screen filled"
exit 0
