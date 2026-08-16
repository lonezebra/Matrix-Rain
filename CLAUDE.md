# Housekeeping — GitHub username rename (2026-08-16)

GitHub account renamed `lonezebra` → `Sharpened-Banana`. As part of that, the screensaver's
`CFBundleIdentifier` changed from `com.lonezebra.MatrixRain` to
`com.sharpenedbanana.MatrixRain` (`macos/Info.plist`, `macos/Sources/Settings.swift`).
Not code-signed with entitlements, so no Apple Developer Portal impact — but:

- macOS treats a changed `CFBundleIdentifier` as a **different** screensaver module. Any
  copy of MatrixRain already installed under the old id needs to be removed from
  System Settings → Screen Saver and re-added after rebuilding.
- Saved settings live under `ScreenSaverDefaults` for the old domain
  (`com.lonezebra.MatrixRain`); they won't carry over. Reconfigure via the settings panel
  after switching, or manually copy the `defaults -currentHost` domain if you want to
  preserve the old values.
