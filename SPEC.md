# Matrix Rain Screensaver — Shared Specification

All three platform implementations (macOS, Windows, Linux) MUST follow this
spec so the rain looks and behaves identically everywhere. Platform code may
be idiomatic (Swift / Win32 C / X11 C) but the simulation model, defaults,
and setting ranges are fixed here.

## 1. Visual model (movie-accurate "digital rain")

The screen is a grid of character cells:

- `cols = floor(screenWidth  / cellWidth)`
- `rows = floor(screenHeight / cellHeight)`
- `cellWidth ≈ 0.62 * glyphSize`, `cellHeight ≈ 1.05 * glyphSize`
  (monospaced vertical columns, slightly tight horizontal packing)

Each **column** hosts zero or more falling **streams**. A stream is defined by:

| Field        | Meaning                                                    |
|--------------|------------------------------------------------------------|
| `headRow`    | fractional row position of the bright head glyph           |
| `speed`      | rows/second, per-stream randomized: `base * uniform(0.6, 1.4)` |
| `length`     | trail length in cells: `uniform(0.35, 0.95) * rows`        |

Behavior each frame (target 30 FPS, time-based so any FPS looks right):

1. `headRow += speed * dt`.
2. The **head** glyph renders in the head color (near-white by default) at
   full brightness — this is the iconic bright leader.
3. Behind the head, glyphs render in the rain color with brightness fading
   linearly from 1.0 (just behind head) to 0.0 at `length` cells back.
   A per-cell brightness buffer (decayed each frame) is an acceptable and
   recommended implementation: on head pass set cell brightness = 1.0,
   decay all cells by `speed / length` rows-worth per second.
4. When `headRow - length > rows` the stream is dead; the column may respawn
   after a random gap (see density).
5. **Glyph mutation**: every cell has a small chance per second
   (`MUTATION_RATE = 1.5` mutations/cell/minute ≈ 0.025/s) of swapping its
   character in place while it is still visible. The head glyph changes
   character every time it advances a row.

### Glyph set

Half-width katakana plus digits and a few latin/symbol characters, matching
the film:

```
ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝ
0123456789
Z:・."=*+-<>¦｜
```

If the platform font cannot supply katakana, fall back to digits + latin
uppercase, but katakana MUST be attempted first (macOS: menlo/monaco system
fallback handles it; Windows: "MS Gothic"; Linux: a bundled/system CJK mono
font via fontconfig, e.g. "Noto Sans Mono CJK JP", falling back to "misc-fixed").
Glyphs may be drawn horizontally mirrored for extra authenticity where cheap
to do (optional).

### Color

- `rainColor` — the trail color. Default matrix green **#00FF41**.
- Head color is derived, not a separate setting: blend `rainColor` 75% toward
  white (`head = rain + (white - rain) * 0.75`).
- Trail brightness scales the rain color toward black. Rendering on pure
  black background (#000000) always.
- Optional subtle glow/bold on the head if cheap on the platform.

## 2. User-adjustable settings

Every platform MUST expose exactly these settings, with these keys, ranges,
and defaults. Storage is platform-idiomatic (ScreenSaverDefaults / registry /
config file) but key names are shared.

| Key         | Type   | Range          | Default   | Meaning                                  |
|-------------|--------|----------------|-----------|------------------------------------------|
| `color`     | RGB    | any            | `#00FF41` | trail color; UI offers presets + custom  |
| `density`   | float  | 0.05 – 1.0     | 0.75      | fraction of columns targeted to be active |
| `size`      | int    | 8 – 64 (px/pt) | 18        | glyph font size; drives cell grid        |
| `speed`     | float  | 0.5 – 3.0      | 1.0       | multiplier on base fall rate             |

Derived constants:

- Base fall rate: `BASE_SPEED = 10.0` rows/second. Effective per-stream speed
  = `BASE_SPEED * speed * uniform(0.6, 1.4)`.
- Density controls spawning: each frame, if
  `activeStreams < density * cols`, spawn a new stream in a random idle
  column with probability `1 - exp(-dt * SPAWN_RATE)` where
  `SPAWN_RATE = 3.0 * cols * density / rows` (streams/sec), head starting
  above the top edge (`headRow = -uniform(0, rows)` allowed so streams
  enter staggered).

### Color presets (offer in every settings UI)

| Name          | Hex       |
|---------------|-----------|
| Matrix Green  | `#00FF41` |
| Amber         | `#FFB000` |
| Ice Blue      | `#00C8FF` |
| Crimson       | `#FF2020` |
| Violet        | `#B040FF` |
| White         | `#E0E0E0` |

Plus a free RGB custom picker where the platform makes that easy.

## 3. Platform packaging requirements

### macOS (`macos/`)
- `.saver` bundle containing a `ScreenSaverView` subclass (Swift).
- Settings via configure sheet (`hasConfigureSheet` / `configureSheet`),
  persisted with `ScreenSaverDefaults(forModuleWithName:)`.
- Buildable with plain `make` (swiftc, no Xcode required) AND include the
  minimal Info.plist. `make install` copies to `~/Library/Screen Savers`.

### Windows (`windows/`)
- Single-file Win32 `.scr` in C. Must handle command lines: `/s` (run),
  `/p <hwnd>` (preview in the small Settings monitor), `/c` (settings
  dialog), and no-args (settings dialog, per convention).
- Settings dialog is a native dialog (dialog template or created in code)
  with presets combo, RGB fields, and sliders/edits for density/size/speed.
- Persist to `HKEY_CURRENT_USER\Software\MatrixRain`.
- GDI double-buffered rendering is sufficient at 30 FPS.
- Buildable with MinGW (`x86_64-w64-mingw32-gcc`) via Makefile and with
  MSVC via a `build.bat`.

### Linux (`linux/`)
- C + Xlib (Xft for antialiased scalable glyphs via fontconfig).
- Runs three ways:
  1. `matrix-rain` — own fullscreen override-redirect window (screensaver
     look), exits on key/mouse.
  2. `matrix-rain -window` — normal window for testing.
  3. `matrix-rain -window-id <id>` / `-root` — draws into an existing
     window; this makes it a drop-in **XScreenSaver hack** and works with
     xwinwrap. Honor `XSCREENSAVER_WINDOW` env var too.
- Settings via CLI flags (`-color`, `-density`, `-size`, `-speed`) and a
  config file `~/.config/matrix-rain/matrix-rain.conf` (`key = value`
  lines, CLI overrides file).
- Ship an XScreenSaver integration snippet and a `.desktop` file.
- `make` / `make install` (PREFIX=/usr/local).

## 4. Quality bar

- Time-based animation (`dt` from a monotonic clock); no fixed-step assumptions.
- Resize/multi-resolution safe: rebuild the grid on size change.
- CPU-friendly: only redraw what changed where the platform makes that easy,
  otherwise full-frame blit at 30 FPS is acceptable.
- No flicker: double-buffer everywhere.
- Clean exit on user input in real screensaver modes (macOS/Windows handle
  this at the OS level; Linux standalone handles it itself).
