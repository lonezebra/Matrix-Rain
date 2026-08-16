# Matrix Rain Screensaver

The digital rain from *The Matrix*, as a native screensaver for **macOS**,
**Windows**, and **Linux** — bright white-hot leader glyphs, fading green
trails of half-width katakana, glyphs mutating in place as they fall.

Every setting you'd want to tweak is adjustable on every platform:

| Setting   | What it does                              | Range      | Default        |
|-----------|-------------------------------------------|------------|----------------|
| Color     | Trail color (head auto-brightens from it) | any RGB    | `#00FF41`      |
| Density   | How many columns are raining              | 5% – 100%  | 75%            |
| Size      | Glyph size (drives the whole grid)        | 8 – 64     | 18             |
| Speed     | Fall-rate multiplier                      | 0.5× – 3×  | 1×             |

Six color presets ship everywhere: Matrix Green, Amber, Ice Blue, Crimson,
Violet, White — plus fully custom RGB.

All three implementations follow one shared spec ([SPEC.md](SPEC.md)) so the
rain looks identical no matter the OS.

## Platforms

| Platform | Tech | Package | Docs |
|----------|------|---------|------|
| macOS    | Swift + ScreenSaver framework | `MatrixRain.saver` bundle | [macos/README.md](macos/README.md) |
| Windows  | C + Win32/GDI | `MatrixRain.scr` | [windows/README.md](windows/README.md) |
| Linux    | C + Xlib/Xft | `matrix-rain` binary (standalone fullscreen **and** XScreenSaver hack) | [linux/README.md](linux/README.md) |

### Quick start

**macOS** (needs Xcode Command Line Tools):
```sh
cd macos && make install
# System Settings → Screen Saver → Matrix Rain → Options…
```

**Windows** (MinGW shown; MSVC via `build.bat`):
```sh
cd windows && make
# then right-click MatrixRain.scr → Install
```

**Linux** (Debian/Ubuntu deps: libx11-dev libxft-dev libfontconfig-dev):
```sh
cd linux && make && sudo make install
matrix-rain            # fullscreen, any key exits
matrix-rain -window    # windowed test mode
```

Settings live where each OS expects them: `ScreenSaverDefaults` on macOS
(configure sheet UI), `HKCU\Software\MatrixRain` on Windows (settings
dialog UI), and `~/.config/matrix-rain/matrix-rain.conf` + CLI flags on
Linux.

## Performance

All three renderers use **dirty-cell rendering**: a cell is only
repainted when its brightness crosses into a new shade level or its
glyph changes, instead of redrawing the whole grid every frame. Each
platform has a `tests/measure_utilization.sh` (PowerShell on Windows)
script that runs the real render loop and fails if CPU usage grows as
the screen fills — see each platform's README "Development" section.
GPU acceleration was evaluated but skipped: the CPU fix already gets
4K frame times under 3ms, so a GPU rewrite wasn't worth the added
per-platform complexity (Metal / Direct2D-Direct3D / OpenGL).

## Repo layout

```
SPEC.md      shared visual/simulation/settings specification
macos/       Swift .saver bundle + Makefile (no Xcode project needed)
             tests/measure_utilization.sh — CPU/GPU utilization regression test
windows/     Win32 C .scr + Makefile (MinGW) + build.bat (MSVC)
             tests/Measure-Utilization.ps1 — CPU/GPU utilization regression test
linux/       Xlib/Xft C source + Makefile + xscreensaver integration
             tests/measure_utilization.sh — CPU/GPU utilization regression test
```
