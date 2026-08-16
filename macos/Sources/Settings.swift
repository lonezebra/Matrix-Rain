import AppKit
import ScreenSaver

/// Typed access to the shared settings (SPEC.md section 2), persisted with
/// ScreenSaverDefaults under the module "com.lonezebra.MatrixRain".
struct Settings {

    static let moduleName = "com.lonezebra.MatrixRain"
    static let defaultColorHex = "#00FF41"

    enum Keys {
        static let color = "color"
        static let density = "density"
        static let size = "size"
        static let speed = "speed"
    }

    static let presets: [(name: String, hex: String)] = [
        ("Matrix Green", "#00FF41"),
        ("Amber", "#FFB000"),
        ("Ice Blue", "#00C8FF"),
        ("Crimson", "#FF2020"),
        ("Violet", "#B040FF"),
        ("White", "#E0E0E0"),
    ]

    private let defaults: UserDefaults

    init() {
        defaults = ScreenSaverDefaults(forModuleWithName: Settings.moduleName) ?? .standard
        defaults.register(defaults: [
            Keys.color: Settings.defaultColorHex,
            Keys.density: 0.75,
            Keys.size: 18,
            Keys.speed: 1.0,
        ])
    }

    /// Trail color as "#RRGGBB" (sRGB); invalid stored values fall back to the default.
    var colorHex: String {
        get {
            let raw = defaults.string(forKey: Keys.color) ?? Settings.defaultColorHex
            return NSColor(hexString: raw) != nil ? raw : Settings.defaultColorHex
        }
        nonmutating set {
            defaults.set(newValue, forKey: Keys.color)
        }
    }

    var color: NSColor {
        get { NSColor(hexString: colorHex) ?? NSColor(srgbRed: 0, green: 1, blue: 0.255, alpha: 1) }
        nonmutating set { colorHex = newValue.srgbHexString }
    }

    var density: Double {
        get { min(max(defaults.double(forKey: Keys.density), 0.05), 1.0) }
        nonmutating set { defaults.set(min(max(newValue, 0.05), 1.0), forKey: Keys.density) }
    }

    var size: Int {
        get { min(max(defaults.integer(forKey: Keys.size), 8), 64) }
        nonmutating set { defaults.set(min(max(newValue, 8), 64), forKey: Keys.size) }
    }

    var speed: Double {
        get { min(max(defaults.double(forKey: Keys.speed), 0.5), 3.0) }
        nonmutating set { defaults.set(min(max(newValue, 0.5), 3.0), forKey: Keys.speed) }
    }

    func synchronize() {
        defaults.synchronize()
    }
}

extension NSColor {

    /// Parses "#RRGGBB" (leading "#" optional) as an sRGB color.
    convenience init?(hexString: String) {
        var s = hexString.trimmingCharacters(in: .whitespaces)
        if s.hasPrefix("#") { s.removeFirst() }
        guard s.count == 6, let v = UInt32(s, radix: 16) else { return nil }
        self.init(srgbRed: CGFloat((v >> 16) & 0xFF) / 255,
                  green: CGFloat((v >> 8) & 0xFF) / 255,
                  blue: CGFloat(v & 0xFF) / 255,
                  alpha: 1)
    }

    /// "#RRGGBB" using explicit sRGB components.
    var srgbHexString: String {
        let c = usingColorSpace(.sRGB) ?? self
        let r = Int((c.redComponent * 255).rounded())
        let g = Int((c.greenComponent * 255).rounded())
        let b = Int((c.blueComponent * 255).rounded())
        return String(format: "#%02X%02X%02X", r, g, b)
    }
}
