import AppKit
import Dispatch
import Foundation
import ScreenSaver

// Standalone command-line host for running a .saver bundle's view outside
// System Settings. Exists because the legacy `ScreenSaverEngine` binary this
// harness used to shell out to has moved or disappeared across recent macOS
// releases (screensaver hosting moved into a System Settings extension) --
// this loads the bundle directly instead, which only depends on the public
// ScreenSaverView contract and needs nothing OS-version-specific.
//
// Usage: PreviewHost <bundle.saver> <width> <height> <duration-seconds>
//
// Prints nothing of its own; MatrixRainView emits "fpslog ..." lines when
// MATRIX_RAIN_FPSLOG=1 is set in the environment.

let args = CommandLine.arguments
guard args.count == 5,
      let width = Double(args[2]), let height = Double(args[3]),
      let duration = Double(args[4]) else {
    FileHandle.standardError.write(
        "usage: PreviewHost <bundle.saver> <width> <height> <duration-seconds>\n".data(using: .utf8)!)
    exit(2)
}

let bundlePath = args[1]
guard let bundle = Bundle(path: bundlePath) else {
    FileHandle.standardError.write("error: cannot open bundle at \(bundlePath)\n".data(using: .utf8)!)
    exit(2)
}
guard bundle.load() else {
    FileHandle.standardError.write(
        "error: bundle failed to load -- check codesigning (codesign --force --sign - MatrixRain.saver) and architecture\n"
            .data(using: .utf8)!)
    exit(2)
}
guard let principalClass = bundle.principalClass as? ScreenSaverView.Type else {
    FileHandle.standardError.write(
        "error: NSPrincipalClass is not a ScreenSaverView subclass (or failed to resolve)\n".data(using: .utf8)!)
    exit(2)
}

let frame = NSRect(x: 0, y: 0, width: width, height: height)
guard let view = principalClass.init(frame: frame, isPreview: false) else {
    FileHandle.standardError.write("error: failed to instantiate \(principalClass)\n".data(using: .utf8)!)
    exit(2)
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)

let window = NSWindow(contentRect: frame, styleMask: [.titled, .closable],
                       backing: .buffered, defer: false)
window.title = "Matrix Rain -- benchmark host"
window.contentView = view
window.makeKeyAndOrderFront(nil)
app.activate(ignoringOtherApps: true)

view.startAnimation()

// ScreenSaverView drives animateOneFrame off a timer it schedules on the
// current run loop when startAnimation() is called with the view attached
// to a window -- app.run() below supplies that run loop.
DispatchQueue.main.asyncAfter(deadline: .now() + duration) {
    view.stopAnimation()
    app.terminate(nil)
}

app.run()
