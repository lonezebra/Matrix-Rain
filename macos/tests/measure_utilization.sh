#!/bin/zsh
# Regression test for the "chugs after a few seconds" bug: runs the real
# MatrixRain.saver in the legacy ScreenSaverEngine preview harness and
# samples CPU% and GPU% over time via `powermetrics`. FAILS if late-run
# usage is significantly higher than early-run usage -- that growth
# pattern is exactly the symptom the dirty-rect invalidation fix (see
# ../Sources/MatrixRainView.swift) was written to kill.
#
# Cannot be executed in the Linux build container this project was
# developed in -- there is no Swift toolchain or macOS kernel here. Run
# this on a real Mac after `make install` in ../.
#
# Requires sudo (powermetrics needs root to read the energy/GPU counters).
#
# Usage: measure_utilization.sh [duration_sec]
#   duration_sec  total sample window (default 60)

set -u
SCRIPT_DIR="$(cd "$(dirname "${(%):-%x}")" && pwd)"
SAVER="$HOME/Library/Screen Savers/MatrixRain.saver"
DURATION="${1:-60}"
SAMPLE_INTERVAL=2
REGRESSION_THRESHOLD_PCT=25

if [ ! -d "$SAVER" ]; then
    echo "error: $SAVER not found -- run 'make install' in macos/ first" >&2
    exit 2
fi

if ! command -v powermetrics >/dev/null 2>&1; then
    echo "error: powermetrics not found (should ship with macOS)" >&2
    exit 2
fi

# System Preferences' ScreenSaverEngine is the same process real screensaver
# previews run under; -background keeps it out of your face while it
# renders our .saver full tilt against a real CAMetalLayer-backed window,
# which is what actually exercises GPU compositing of our CGImage blits.
ENGINE=/System/Library/Frameworks/ScreenSaver.framework/Resources/ScreenSaverEngine.app/Contents/MacOS/ScreenSaverEngine
if [ ! -x "$ENGINE" ]; then
    echo "error: ScreenSaverEngine not found at expected path -- macOS layout may have changed" >&2
    exit 2
fi

cleanup() {
    [ -n "${ENGINE_PID:-}" ] && kill "$ENGINE_PID" 2>/dev/null
    [ -n "${PM_PID:-}" ] && kill "$PM_PID" 2>/dev/null
}
trap cleanup EXIT INT TERM

echo "launching ScreenSaverEngine with MatrixRain.saver for ${DURATION}s"
"$ENGINE" -module MatrixRain -moduleName MatrixRain -background &
ENGINE_PID=$!
sleep 3

PM_OUT=$(mktemp)
echo "note: powermetrics requires sudo -- you may be prompted for your password"
sudo powermetrics --samplers cpu_power,gpu_power -i "$((SAMPLE_INTERVAL * 1000))" \
    -n "$((DURATION / SAMPLE_INTERVAL))" > "$PM_OUT" &
PM_PID=$!
wait "$PM_PID"

osascript -e 'tell application "ScreenSaverEngine" to quit' 2>/dev/null
kill "$ENGINE_PID" 2>/dev/null
ENGINE_PID=""

# powermetrics prints one block per sample; pull the aggregate "CPU Power"
# active-residency percentage and "GPU active residency" line from each.
cpu_vals=("${(@f)$(grep -E '^CPU Power' "$PM_OUT" | awk '{print $3}')}")
gpu_vals=("${(@f)$(grep -E 'GPU HW active residency' "$PM_OUT" | grep -oE '[0-9.]+%' | tr -d '%')}")

echo "note: raw powermetrics output kept at $PM_OUT"

count=${#cpu_vals[@]}
if [ "$count" -lt 3 ]; then
    echo "warning: too few samples parsed from powermetrics output ($count) -- check $PM_OUT manually"
    exit 0
fi

third=$((count / 3))
early_sum=0
for ((i = 1; i <= third; i++)); do early_sum=$((early_sum + cpu_vals[i])); done
early_avg=$(echo "scale=2; $early_sum / $third" | bc)

late_sum=0
for ((i = count - third + 1; i <= count; i++)); do late_sum=$((late_sum + cpu_vals[i])); done
late_avg=$(echo "scale=2; $late_sum / $third" | bc)

echo
echo "early-window avg CPU active residency: ${early_avg}%  (screen still filling)"
echo "late-window  avg CPU active residency: ${late_avg}%  (steady-state)"
[ "$count" -le "${#gpu_vals[@]}" ] && echo "GPU active residency samples: ${gpu_vals[*]}"

if [ "$early_avg" = "0" ] || [ -z "$early_avg" ]; then
    echo "note: early-window CPU% was 0 -- skipping growth ratio"
    exit 0
fi

growth=$(echo "scale=2; 100 * ($late_avg - $early_avg) / $early_avg" | bc)
echo "growth: ${growth}%  (fail threshold: +${REGRESSION_THRESHOLD_PCT}%)"

if (( $(echo "$growth > $REGRESSION_THRESHOLD_PCT" | bc -l) )); then
    echo "FAIL: CPU usage grew as the screen filled -- this is the chugging regression"
    exit 1
fi
echo "PASS: CPU usage stayed flat as the screen filled"
exit 0
