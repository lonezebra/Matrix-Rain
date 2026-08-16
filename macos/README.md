# Matrix Rain — macOS Screensaver

Movie-accurate digital rain as a native macOS `.saver` bundle, implementing
the shared model in [`../SPEC.md`](../SPEC.md).

## Requirements

- macOS 12.0 or later
- Xcode Command Line Tools (`xcode-select --install`) — provides `swiftc`,
  `lipo`, and `codesign`. A full Xcode install is not required.

## Build

```sh
cd macos
make            # universal binary (arm64 + x86_64), with single-arch fallback
make ARCH=arm64 # or force a single architecture
```

This produces `MatrixRain.saver` in the current directory. The binary is
built with plain `swiftc -emit-library` (a dylib, which macOS happily loads
as a bundle executable via `dlopen`) and ad-hoc codesigned.

## Install

```sh
make install    # copies MatrixRain.saver to ~/Library/Screen Savers/
```

Then pick it in **System Settings → Screen Saver** (macOS 13+) or
**System Preferences → Desktop & Screen Saver** (macOS 12).

### First-run approval

The bundle is ad-hoc signed (not notarized), so macOS may refuse to load it
on first use. If the saver does not appear or shows a warning:

- Approve it under **System Settings → Privacy & Security** ("Open Anyway"
  after the first blocked attempt), or
- Right-click `MatrixRain.saver` in Finder and choose **Open** to register
  the approval, then re-open Screen Saver settings.

macOS runs third-party savers in the legacy `legacyScreenSaver` host; this
bundle targets that engine. On macOS 14+ System Settings still loads
third-party `.saver` bundles through the same host — if the preview looks
stale after reinstalling, quit System Settings (and
`legacyScreenSaver` under Activity Monitor) and reopen it.

## Settings

Open **System Settings → Screen Saver**, select Matrix Rain, and click
**Options…** to open the configure sheet:

- **Preset** — Matrix Green, Amber, Ice Blue, Crimson, Violet, White, or Custom
- **Color** — free custom trail color via the color well
- **Density** — 0.05–1.0, default 0.75 (fraction of active columns)
- **Glyph size** — 8–64 pt, default 18
- **Speed** — 0.5x–3.0x, default 1.0x

Settings persist via `ScreenSaverDefaults` under
`com.lonezebra.MatrixRain` and take effect immediately on OK.

## Uninstall

```sh
rm -rf ~/Library/Screen\ Savers/MatrixRain.saver
defaults -currentHost delete com.lonezebra.MatrixRain 2>/dev/null || true
```

(Then pick a different saver in System Settings.)

## Development

### CPU/GPU/framerate regression test

`tests/measure_utilization.sh` runs `make` for you first (so it always
measures the bundle matching your current source, never a stale build
left over from earlier — no need to run it separately, and no need for
`make install`), then runs `MatrixRain.saver` inside
`tests/PreviewHost.swift`, a small standalone AppKit host built on first
use. That replaces an earlier version of this script that shelled out to
the legacy `ScreenSaverEngine` binary — that binary has moved or
disappeared on recent macOS releases (screensaver hosting moved into a
System Settings extension), so PreviewHost loads the bundle directly
instead, which only depends on the public `ScreenSaverView` contract.

It samples every 2s for the given duration:
- **CPU%** via `ps` — no privileges needed.
- **fps** — `MatrixRainView` prints its real achieved framerate when
  `MATRIX_RAIN_FPSLOG=1` is set (which the script sets), independent of
  the animation timer's 30 fps target.
- **GPU active residency** via `powermetrics`, system-wide — this
  *does* need `sudo`; if you decline the prompt, the script still runs
  and reports GPU as unavailable, since CPU%/fps are the primary
  regression signal.

It fails if late-run CPU% is more than 25% higher than early-run CPU%
— the "chugs after a few seconds" pattern the dirty-rect invalidation
fix targets.

```sh
./tests/measure_utilization.sh 60   # duration in seconds
```

If `PreviewHost` fails to build, you likely need
`xcode-select --install` (see Requirements above). If it builds but the
bundle fails to load, re-run `codesign --force --sign - MatrixRain.saver`
(ad-hoc signing can be invalidated by editing files after `make`).
