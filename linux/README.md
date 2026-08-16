# Matrix Rain — Linux (X11)

Movie-accurate Matrix digital rain screensaver in C11 using Xlib + Xft
(fontconfig), flicker-free via a persistent Pixmap back buffer with
dirty-cell rendering (only cells whose appearance changed are redrawn
each frame, in batched draw calls).
Implements the shared simulation model in [`../SPEC.md`](../SPEC.md).

## Dependencies

Build needs a C compiler, `make`, `pkg-config`, and dev headers for
X11, Xft, and fontconfig. A CJK monospace font (Noto Sans Mono
CJK JP) gives the authentic half-width katakana glyphs; without one the
program falls back to digits + latin.

**Debian / Ubuntu**

```sh
sudo apt install build-essential pkg-config libx11-dev libxft-dev libfontconfig-dev fonts-noto-cjk
```

**Fedora**

```sh
sudo dnf install gcc make pkgconf-pkg-config libX11-devel libXft-devel fontconfig-devel google-noto-sans-mono-cjk-jp-fonts
```

**Arch**

```sh
sudo pacman -S base-devel libx11 libxft fontconfig noto-fonts-cjk
```

## Build & install

```sh
make
sudo make install            # PREFIX=/usr/local by default
make install PREFIX=$HOME/.local   # user install, no sudo
```

Installs the `matrix-rain` binary and a `.desktop` launcher.

## Usage

```sh
matrix-rain                  # fullscreen screensaver; any key/mouse press exits
matrix-rain -window          # normal resizable window (testing); Esc or q quits
matrix-rain -root            # draw on the root window
matrix-rain -window-id 0x1a00003   # draw into an existing window
```

The `XSCREENSAVER_WINDOW` environment variable is honored, which makes
matrix-rain a drop-in xscreensaver hack (see below).

Settings flags (all optional, CLI overrides the config file):

| Flag       | Range     | Default | Meaning |
|------------|-----------|---------|---------|
| `-color`   | `#RRGGBB` or preset | `#00FF41` | trail color |
| `-density` | 0.05 – 1.0 | 0.75   | fraction of columns active |
| `-size`    | 8 – 64     | 18     | glyph size in px |
| `-speed`   | 0.5 – 3.0  | 1.0    | fall speed multiplier |
| `-fps`     | 1 – 240    | 30     | target frame rate |
| `-font`    | fontconfig name | Noto Sans Mono CJK JP, then monospace | glyph font |

Color presets (case-insensitive, spaces optional): `Matrix Green`,
`Amber`, `Ice Blue`, `Crimson`, `Violet`, `White`.

Out-of-range values are clamped with a warning.

## Config file

`~/.config/matrix-rain/matrix-rain.conf` (or
`$XDG_CONFIG_HOME/matrix-rain/matrix-rain.conf`), simple `key = value`
lines, `#` starts a comment:

```ini
# Matrix Rain settings — CLI flags override these
color   = #00FF41      # or a preset name, e.g. Ice Blue
density = 0.75
size    = 18
speed   = 1.0
fps     = 30
# font  = Noto Sans Mono CJK JP
```

## XScreenSaver integration

See [`xscreensaver-snippet.txt`](xscreensaver-snippet.txt) — add
`matrix-rain -root` to the `programs:` list in `~/.xscreensaver` and
restart the daemon. Also works with `xwinwrap` for animated wallpaper.

## Wayland note

This is an X11 program. On a Wayland session it runs only under
XWayland, and there only as a normal window (`matrix-rain -window`) —
fullscreen override-redirect grabbing, `-root`, and xscreensaver-style
locking are X11 concepts that Wayland compositors do not allow.
Wayland-native idle/lock integration (swayidle, ext-session-lock, etc.)
is out of scope here.

## Development

`matrix-rain -selftest` runs the display-independent simulation core
(`rain.c`) headless for 300 frames and prints stream/cell statistics —
useful for CI or machines without an X display.

`matrix-rain -window -bench <seconds>` runs the real X render path at
full display resolution with the FPS cap removed (simulation still
advances in real time), then prints total frames, average/max frame
render time, and average glyph-draw-calls per frame. Handy under Xvfb
for regression-checking render performance, e.g.:

```sh
xvfb-run -s "-screen 0 1920x1080x24" ./matrix-rain -window -bench 20
```

### CPU/GPU utilization regression test

`tests/measure_utilization.sh` runs the real fps-capped renderer for a
while and samples process CPU% (from `/proc/<pid>/stat`) every 2s, plus
GPU% if `nvidia-smi`/`intel_gpu_top`/`radeontop` is installed. It fails
if late-run CPU% is more than 25% higher than early-run CPU% — that
growth pattern is exactly the "chugs after a few seconds" symptom the
dirty-cell rendering fix targets. GPU% reading `n/a` or ~0% is expected
and not a failure (Xft/XRender rendering here is CPU-side).

```sh
./tests/measure_utilization.sh 60 1920 1080   # duration, width, height
```

Uses Xvfb automatically if `$DISPLAY` isn't already set.
