# Matrix Rain — Windows screensaver

Single-file Win32 `.scr` implementing the shared spec in `../SPEC.md`:
movie-accurate digital rain (half-width katakana + digits + symbols, bright
head, linear-fading trails, glyph mutation), GDI double-buffered at 30 FPS,
one topmost window per monitor.

## Build

### MinGW (Linux / WSL cross-compile, or native MinGW/MSYS2)

```sh
cd windows
make                # cross: x86_64-w64-mingw32-gcc
make CC=gcc         # native MinGW
```

Produces `MatrixRain.scr`.

### MSVC

Open a *Developer Command Prompt for VS* (or run `vcvars64.bat`), then:

```bat
cd windows
build.bat
```

## Install

Modern, recommended way:

1. Right-click `MatrixRain.scr` in Explorer and choose **Install**.
2. Windows opens Screen Saver Settings with Matrix Rain selected; set the
   wait time and click OK.

Alternatively copy the `.scr` into `C:\Windows\System32` (64-bit build) so it
appears in the Screen Saver Settings list permanently. (Only a 32-bit build
would go in `SysWOW64`.) Copying to System32 needs admin rights; the
right-click Install method needs none and works from any folder.

To test without installing:

```bat
MatrixRain.scr /s      # run fullscreen now (any key/click/mouse move exits)
MatrixRain.scr /c      # open the settings dialog
MatrixRain.scr /t      # headless simulation self-test (300 frames, prints stats)
```

## Settings

Run the screensaver's **Settings…** button in Screen Saver Settings (or
`MatrixRain.scr /c`). The dialog offers:

- **Preset** — Matrix Green, Amber, Ice Blue, Crimson, Violet, White, plus
  *Custom* (selected automatically when you edit the RGB fields).
- **Red / Green / Blue** — free 0–255 trail color. The head color is derived
  (blended 75% toward white).
- **Density** — 5–100% of columns targeted to be raining (default 75%).
- **Glyph size** — 8–64 px (default 18).
- **Speed** — 50–300% of the base fall rate (default 100%).

Values persist in the registry under `HKEY_CURRENT_USER\Software\MatrixRain`
(`color` as a COLORREF DWORD, `density`/`speed` as percent DWORDs, `size` as a
DWORD). Out-of-range values are clamped on load.

Glyphs render with **MS Gothic** (ships with Windows and includes the
half-width katakana); if unavailable it falls back to Consolas, then the stock
fixed font.

## Uninstall

- If you used right-click **Install**: pick a different screensaver in Screen
  Saver Settings, then delete `MatrixRain.scr` from wherever you kept it.
- If you copied it to `System32`: delete `C:\Windows\System32\MatrixRain.scr`
  (admin rights required).
- To remove saved settings: delete the registry key
  `HKEY_CURRENT_USER\Software\MatrixRain` (e.g.
  `reg delete HKCU\Software\MatrixRain /f`).
