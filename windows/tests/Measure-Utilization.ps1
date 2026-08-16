#Requires -Version 5.1
<#
.SYNOPSIS
    Regression test for the "chugs after a few seconds" bug: runs the real
    MatrixRain.scr in windowed/preview-style mode and samples process CPU%
    and per-process GPU engine utilization over time. FAILS if late-run
    usage is significantly higher than early-run usage -- that growth
    pattern is exactly the symptom the glyph-atlas + dirty-cell-BitBlt fix
    (see ../matrix_rain.c) was written to kill.

.DESCRIPTION
    Cannot be executed in the Linux build container this project was
    developed in -- run it on real Windows 10/11. Uses the built-in
    Get-Counter cmdlet, so no extra tooling is required:
      - '\Process(MatrixRain)\% Processor Time'   (CPU, divided by core count)
      - '\GPU Engine(*MatrixRain*)\Utilization Percentage'   (GPU, sum of engines)
    GPU Engine counters were added in Windows 10 1803+; on older systems
    the GPU column reports "n/a" and that's an expected, non-failing result
    (GDI text rendering is CPU-side rasterization -- near-zero GPU load is
    the correct baseline, not a gap in the test).

.PARAMETER DurationSeconds
    Total sample window. Default 60.

.PARAMETER SampleIntervalSeconds
    Seconds between samples. Default 2.

.PARAMETER RegressionThresholdPercent
    Late-window average may exceed early-window average by at most this
    many percent before the test fails. Default 25.

.EXAMPLE
    .\Measure-Utilization.ps1
    .\Measure-Utilization.ps1 -DurationSeconds 120 -RegressionThresholdPercent 15
#>
param(
    [int]$DurationSeconds = 60,
    [int]$SampleIntervalSeconds = 2,
    [int]$RegressionThresholdPercent = 25
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $scriptDir '..\MatrixRain.scr'

if (-not (Test-Path $exe)) {
    Write-Error "MatrixRain.scr not found at $exe -- build it first (see ../README.md)"
    exit 2
}

Write-Host "launching $exe /s for $DurationSeconds s"
# /s runs the real fullscreen path with the real 30 FPS timer and real
# GDI atlas/dirty-cell rendering -- the same code path a live screensaver
# uses, unlike the headless /t self-test.
$proc = Start-Process -FilePath $exe -ArgumentList '/s' -PassThru
Start-Sleep -Seconds 1

if ($proc.HasExited) {
    Write-Error "MatrixRain.scr exited immediately (exit code $($proc.ExitCode))"
    exit 2
}

$coreCount = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
$samples = [int]($DurationSeconds / $SampleIntervalSeconds)
$cpuSamples = @()
$gpuSamples = @()

$gpuCounterPath = "\GPU Engine(*)\Utilization Percentage"
$gpuAvailable = $true
try {
    Get-Counter -Counter $gpuCounterPath -ErrorAction Stop | Out-Null
} catch {
    $gpuAvailable = $false
    Write-Host "note: GPU Engine performance counters unavailable on this system -- GPU column will read n/a"
}

Write-Host ("{0,6}  {1,8}  {2,8}" -f "t(s)", "cpu%", "gpu%")
for ($i = 1; $i -le $samples; $i++) {
    Start-Sleep -Seconds $SampleIntervalSeconds

    if ($proc.HasExited) {
        Write-Error "MatrixRain.scr died during sampling (sample $i) -- possible crash under load"
        exit 2
    }

    try {
        $cpuRaw = (Get-Counter "\Process(MatrixRain)\% Processor Time" -ErrorAction Stop).CounterSamples[0].CookedValue
        $cpuPct = [math]::Round($cpuRaw / $coreCount, 2)
    } catch {
        $cpuPct = $null
    }

    $gpuPct = $null
    if ($gpuAvailable) {
        try {
            $gpuSum = (Get-Counter $gpuCounterPath -ErrorAction Stop).CounterSamples |
                Where-Object { $_.InstanceName -match 'matrixrain' } |
                Measure-Object -Property CookedValue -Sum
            $gpuPct = [math]::Round($gpuSum.Sum, 2)
        } catch {
            $gpuPct = $null
        }
    }

    $t = $i * $SampleIntervalSeconds
    $cpuDisplay = if ($null -ne $cpuPct) { $cpuPct } else { "n/a" }
    $gpuDisplay = if ($null -ne $gpuPct) { $gpuPct } else { "n/a" }
    Write-Host ("{0,6}  {1,8}  {2,8}" -f $t, $cpuDisplay, $gpuDisplay)

    if ($null -ne $cpuPct) { $cpuSamples += $cpuPct }
    if ($null -ne $gpuPct) { $gpuSamples += $gpuPct }
}

Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue

$third = [int]($cpuSamples.Count / 3)
if ($third -lt 1) {
    Write-Host "warning: duration too short for a meaningful early/late comparison"
    exit 0
}

$earlyAvg = ($cpuSamples[0..($third - 1)] | Measure-Object -Average).Average
$lateAvg = ($cpuSamples[($cpuSamples.Count - $third)..($cpuSamples.Count - 1)] | Measure-Object -Average).Average

Write-Host ""
Write-Host "early-window avg CPU%: $([math]::Round($earlyAvg,2))  (screen still filling)"
Write-Host "late-window  avg CPU%: $([math]::Round($lateAvg,2))  (steady-state)"

if ($earlyAvg -eq 0) {
    Write-Host "note: early-window CPU% was 0 -- skipping growth ratio (would divide by zero)"
    exit 0
}

$growth = [math]::Round(100 * ($lateAvg - $earlyAvg) / $earlyAvg, 2)
Write-Host "growth: $growth%  (fail threshold: +$RegressionThresholdPercent%)"

if ($growth -gt $RegressionThresholdPercent) {
    Write-Host "FAIL: CPU usage grew as the screen filled -- this is the chugging regression"
    exit 1
}
Write-Host "PASS: CPU usage stayed flat as the screen filled"
exit 0
