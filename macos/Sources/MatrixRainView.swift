import AppKit
import ScreenSaver
import QuartzCore

@objc(MatrixRainView)
final class MatrixRainView: ScreenSaverView {

    /// Number of trail brightness buckets in the atlas; one extra level on
    /// top of these holds the head color.
    private static let trailLevels = 24

    private let settings = Settings()
    private var simulation = RainSimulation(cols: 1, rows: 1, glyphCount: 1)
    private var glyphSet: [Character] = RainSimulation.fullGlyphSet
    private var glyphFont = NSFont.monospacedSystemFont(ofSize: 18, weight: .regular)
    private var cellSize = CGSize(width: 12, height: 20)
    private var atlas: GlyphAtlas?
    private var lastFrameTime: CFTimeInterval = 0
    private var sheetController: ConfigureSheetController?

    override init?(frame: NSRect, isPreview: Bool) {
        super.init(frame: frame, isPreview: isPreview)
        commonInit()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        commonInit()
    }

    private func commonInit() {
        animationTimeInterval = 1.0 / 30.0
        applySettings()
    }

    // MARK: - Animation

    override func startAnimation() {
        super.startAnimation()
        lastFrameTime = 0
        applySettings()
    }

    override func animateOneFrame() {
        let now = CACurrentMediaTime()
        // Real elapsed time, clamped so a long stall doesn't fast-forward.
        let dt = lastFrameTime == 0 ? animationTimeInterval : min(now - lastFrameTime, 0.5)
        lastFrameTime = now
        simulation.update(dt: dt)
        needsDisplay = true
    }

    // MARK: - Drawing

    override func draw(_ rect: NSRect) {
        NSColor.black.setFill()
        bounds.fill()

        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        if atlas == nil { atlas = buildAtlas() }
        guard let atlas = atlas else { return }

        ctx.interpolationQuality = .none
        let cw = cellSize.width
        let ch = cellSize.height
        let height = bounds.height
        let cols = simulation.cols
        let rows = simulation.rows

        for col in 0..<cols {
            let headRow = simulation.headRow(inColumn: col)
            for row in 0..<rows {
                let i = row * cols + col
                let level: Int
                if row == headRow {
                    level = Self.trailLevels
                } else {
                    let b = simulation.brightness[i]
                    if b <= 0 { continue }
                    level = min(Self.trailLevels - 1, Int(b * Float(Self.trailLevels)))
                }
                let tile = atlas.tiles[level][simulation.glyph[i]]
                let cellRect = CGRect(x: CGFloat(col) * cw,
                                      y: height - CGFloat(row + 1) * ch,
                                      width: cw,
                                      height: ch)
                ctx.draw(tile, in: cellRect)
            }
        }
    }

    // MARK: - Layout

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        rebuildGrid()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        atlas = nil
        needsDisplay = true
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        atlas = nil
        needsDisplay = true
    }

    // MARK: - Settings

    /// Re-reads all settings and rebuilds the font, atlas, and grid.
    /// Called at init, animation start, and after the configure sheet saves.
    private func applySettings() {
        let size = CGFloat(settings.size)
        let (font, glyphs) = MatrixRainView.pickFont(size: size)
        glyphFont = font
        glyphSet = glyphs
        cellSize = CGSize(width: 0.62 * size, height: 1.05 * size)
        simulation.glyphCount = glyphs.count
        simulation.density = settings.density
        simulation.speed = settings.speed
        atlas = nil
        rebuildGrid()
        needsDisplay = true
    }

    private func rebuildGrid() {
        let cols = max(1, Int(bounds.width / cellSize.width))
        let rows = max(1, Int(bounds.height / cellSize.height))
        if cols != simulation.cols || rows != simulation.rows {
            simulation.resize(cols: cols, rows: rows)
        }
    }

    // MARK: - Font selection

    /// Monospaced system font does not cover katakana, so prefer fonts that
    /// do; verify via coveredCharacterSet and drop uncovered glyphs.
    private static func pickFont(size: CGFloat) -> (NSFont, [Character]) {
        let katakana = Array(RainSimulation.katakana)
        let candidates = ["Osaka-Mono", "HiraKakuProN-W3", "Hiragino Kaku Gothic ProN"]
        for name in candidates {
            if let font = NSFont(name: name, size: size), covers(font, katakana) {
                let usable = RainSimulation.fullGlyphSet.filter { covers(font, [$0]) }
                if !usable.isEmpty { return (font, usable) }
            }
        }
        let font = NSFont.monospacedSystemFont(ofSize: size, weight: .regular)
        let usable = RainSimulation.fallbackGlyphSet.filter { covers(font, [$0]) }
        return (font, usable.isEmpty ? RainSimulation.fallbackGlyphSet : usable)
    }

    private static func covers(_ font: NSFont, _ chars: [Character]) -> Bool {
        // Rebuild as a value-type CharacterSet via the bitmap so this compiles
        // whether coveredCharacterSet is surfaced as NSCharacterSet or CharacterSet.
        let set = CharacterSet(bitmapRepresentation: font.coveredCharacterSet.bitmapRepresentation)
        return chars.allSatisfy { ch in
            ch.unicodeScalars.allSatisfy { set.contains($0) }
        }
    }

    // MARK: - Glyph atlas

    private struct GlyphAtlas {
        /// tiles[level][glyphIndex]; levels 0..<trailLevels are the fading
        /// trail, level trailLevels is the head color.
        let tiles: [[CGImage]]
    }

    /// Renders every glyph once per brightness level into a single bitmap,
    /// then crops per-tile CGImages so each frame is pure blits.
    private func buildAtlas() -> GlyphAtlas? {
        let scale = window?.backingScaleFactor ?? 2.0
        let levels = Self.trailLevels + 1
        let tileW = Int((cellSize.width * scale).rounded(.up))
        let tileH = Int((cellSize.height * scale).rounded(.up))
        let width = tileW * glyphSet.count
        let height = tileH * levels
        guard width > 0, height > 0,
              let space = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: nil,
                                  width: width,
                                  height: height,
                                  bitsPerComponent: 8,
                                  bytesPerRow: 0,
                                  space: space,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }

        guard let rain = settings.color.usingColorSpace(.sRGB) else { return nil }
        let r = rain.redComponent, g = rain.greenComponent, b = rain.blueComponent

        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)

        for level in 0..<levels {
            let color: NSColor
            if level == Self.trailLevels {
                // Head: rain blended 75% toward white.
                color = NSColor(srgbRed: r + (1 - r) * 0.75,
                                green: g + (1 - g) * 0.75,
                                blue: b + (1 - b) * 0.75,
                                alpha: 1)
            } else {
                // Trail: rain scaled toward black.
                let f = CGFloat(level + 1) / CGFloat(Self.trailLevels)
                color = NSColor(srgbRed: r * f, green: g * f, blue: b * f, alpha: 1)
            }
            let attrs: [NSAttributedString.Key: Any] = [.font: glyphFont, .foregroundColor: color]
            for (index, ch) in glyphSet.enumerated() {
                let str = String(ch) as NSString
                let glyphSize = str.size(withAttributes: attrs)
                ctx.saveGState()
                ctx.translateBy(x: CGFloat(index * tileW), y: CGFloat(level * tileH))
                ctx.scaleBy(x: scale, y: scale)
                str.draw(at: CGPoint(x: (cellSize.width - glyphSize.width) / 2,
                                     y: (cellSize.height - glyphSize.height) / 2),
                         withAttributes: attrs)
                ctx.restoreGState()
            }
        }

        NSGraphicsContext.restoreGraphicsState()
        guard let sheet = ctx.makeImage() else { return nil }

        var tiles: [[CGImage]] = []
        for level in 0..<levels {
            var row: [CGImage] = []
            for index in 0..<glyphSet.count {
                // CGImage cropping is top-left origin; context level 0 was
                // drawn at the bottom of the bitmap.
                let rect = CGRect(x: index * tileW,
                                  y: height - (level + 1) * tileH,
                                  width: tileW,
                                  height: tileH)
                guard let tile = sheet.cropping(to: rect) else { return nil }
                row.append(tile)
            }
            tiles.append(row)
        }
        return GlyphAtlas(tiles: tiles)
    }

    // MARK: - Configure sheet

    override var hasConfigureSheet: Bool {
        return true
    }

    override var configureSheet: NSWindow? {
        if sheetController == nil {
            sheetController = ConfigureSheetController(settings: settings) { [weak self] in
                self?.applySettings()
            }
        }
        sheetController?.reload()
        return sheetController?.window
    }
}
