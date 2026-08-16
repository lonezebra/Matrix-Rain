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

    /// Last-drawn quantized state per cell (see RainSimulation.packedState;
    /// -1 = dark). Emptied to force a resync + full repaint.
    private var cellState: [Int32] = []
    /// Reused scratch buffer of dirty cell indices; cleared each frame with
    /// capacity kept, so steady-state frames allocate nothing.
    private var dirtyCells: [Int] = []
    /// One-shot: next animateOneFrame invalidates the whole view instead of
    /// per-cell rects (first frame, resize, settings/backing change).
    private var needsFullRedraw = true

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

    /// draw(_:) paints every pixel of the dirty region (black fill + blits),
    /// so the view is opaque; declaring it lets AppKit skip compositing
    /// whatever is behind us.
    override var isOpaque: Bool { true }

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

        // Diff quantized cell state and invalidate only what changed. Cells
        // whose float brightness moved within the same atlas bucket are not
        // reported and therefore never repainted.
        dirtyCells.removeAll(keepingCapacity: true)
        simulation.syncDisplayState(levels: Self.trailLevels,
                                    state: &cellState,
                                    dirtyCells: &dirtyCells)
        if needsFullRedraw {
            needsFullRedraw = false
            setNeedsDisplay(bounds)
        } else {
            invalidateDirtyRuns()
        }
    }

    /// Coalesces this frame's dirty cells into one rect per contiguous
    /// vertical run within a column and invalidates just those rects; AppKit
    /// unions them into the window's dirty region. syncDisplayState emits
    /// indices column-by-column with rows ascending, which is exactly the
    /// order this linear scan needs.
    private func invalidateDirtyRuns() {
        guard !dirtyCells.isEmpty else { return }
        let cols = simulation.cols
        var runCol = -1
        var runStart = 0
        var runEnd = 0
        for index in dirtyCells {
            let col = index % cols
            let row = index / cols
            if col == runCol && row == runEnd + 1 {
                runEnd = row
            } else {
                if runCol >= 0 {
                    setNeedsDisplay(runRect(col: runCol, rowStart: runStart, rowEnd: runEnd))
                }
                runCol = col
                runStart = row
                runEnd = row
            }
        }
        setNeedsDisplay(runRect(col: runCol, rowStart: runStart, rowEnd: runEnd))
    }

    /// Rect covering rows `rowStart...rowEnd` of `col` in this non-flipped
    /// view (grid row 0 is the top row, so its rect top edge is
    /// bounds.height). This is the single source of the row -> y transform:
    /// drawing uses runRect(col:row:row:) for each cell, so invalidation and
    /// painting can never disagree.
    private func runRect(col: Int, rowStart: Int, rowEnd: Int) -> NSRect {
        let cw = cellSize.width
        let ch = cellSize.height
        return NSRect(x: CGFloat(col) * cw,
                      y: bounds.height - CGFloat(rowEnd + 1) * ch,
                      width: cw,
                      height: CGFloat(rowEnd - rowStart + 1) * ch)
    }

    // MARK: - Drawing

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        if atlas == nil { atlas = buildAtlas() }

        // A system-initiated draw (first display, expose) can arrive before
        // the animation loop has built the snapshot; sync here so drawing
        // always reads current state.
        if cellState.count != simulation.cols * simulation.rows {
            dirtyCells.removeAll(keepingCapacity: true)
            simulation.syncDisplayState(levels: Self.trailLevels,
                                        state: &cellState,
                                        dirtyCells: &dirtyCells)
        }

        // Cell rects are aligned to device pixels (see applySettings) and
        // tiles are baked at exactly cell * backingScaleFactor pixels, so
        // every blit is 1:1 with no resampling.
        ctx.interpolationQuality = .none

        // Paint each rect of the dirty region separately rather than the
        // single bounding-box parameter: with per-column invalidation rects
        // the union box of two dirty cells in distant corners would cover
        // most of the grid and reintroduce the full-grid blit cost. Each
        // region rect is filled black and every cell it touches is reblitted
        // from the snapshot (a cell inside a region rect but not itself dirty
        // is repainted from current — unchanged — state, which is correct,
        // just slightly more work). The rects are disjoint and each pass
        // clips to its own rect, so no pixel is composited twice — important
        // because tiles have transparent backgrounds and double-blitting
        // would brighten antialiased edges.
        var regionRects: UnsafePointer<NSRect>? = nil
        var regionCount = 0
        getRectsBeingDrawn(&regionRects, count: &regionCount)
        if let rects = regionRects, regionCount > 0 {
            for k in 0..<regionCount {
                drawCells(in: rects[k], ctx: ctx)
            }
        } else {
            drawCells(in: dirtyRect, ctx: ctx)
        }
    }

    /// Fills `rect` black and blits every cell intersecting it from the
    /// current snapshot, clipped to `rect`.
    private func drawCells(in rect: NSRect, ctx: CGContext) {
        ctx.saveGState()
        defer { ctx.restoreGState() }
        ctx.clip(to: rect)
        ctx.setFillColor(red: 0, green: 0, blue: 0, alpha: 1)
        ctx.fill(rect)

        guard let atlas = atlas else { return }
        let cols = simulation.cols
        let rows = simulation.rows
        let cw = cellSize.width
        let ch = cellSize.height
        let height = bounds.height

        // Invert the runRect transform to get the covered cell range. Row r
        // spans y in [height - (r+1)*ch, height - r*ch], so rect.maxY bounds
        // the smallest row and rect.minY the largest. floor/ceil err on the
        // side of including boundary-touching cells; extras are repainted
        // from current state, which is harmless.
        let minCol = max(0, Int((rect.minX / cw).rounded(.down)))
        let maxCol = min(cols - 1, Int((rect.maxX / cw).rounded(.up)) - 1)
        let minRow = max(0, Int(((height - rect.maxY) / ch).rounded(.down)))
        let maxRow = min(rows - 1, Int(((height - rect.minY) / ch).rounded(.up)) - 1)
        guard minCol <= maxCol, minRow <= maxRow else { return }

        let tiles = atlas.tiles
        for col in minCol...maxCol {
            for row in minRow...maxRow {
                let packed = cellState[row * cols + col]
                if packed < 0 { continue }
                let level = Int(packed >> 16)
                let glyphIndex = Int(packed & 0xFFFF)
                guard level < tiles.count, glyphIndex < tiles[level].count else { continue }
                ctx.draw(tiles[level][glyphIndex],
                         in: CGRect(x: CGFloat(col) * cw,
                                    y: height - CGFloat(row + 1) * ch,
                                    width: cw,
                                    height: ch))
            }
        }
    }

    // MARK: - Layout

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        rebuildGrid()
        // Even when the grid dimensions survive the resize, bounds.height
        // changed, so every cell's y moved: repaint everything once.
        forceFullRedraw()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        // Cell pixel-alignment and the atlas both depend on backingScaleFactor.
        applySettings()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        applySettings()
    }

    /// Drops the drawn-state snapshot and schedules a one-shot whole-view
    /// repaint (the empty snapshot also makes draw(_:) resync before painting).
    private func forceFullRedraw() {
        cellState.removeAll()
        needsFullRedraw = true
        needsDisplay = true
    }

    // MARK: - Settings

    /// Re-reads all settings and rebuilds the font, atlas, and grid.
    /// Called at init, animation start, window/backing changes, and after the
    /// configure sheet saves.
    private func applySettings() {
        let size = CGFloat(settings.size)
        let (font, glyphs) = MatrixRainView.pickFont(size: size)
        glyphFont = font
        glyphSet = glyphs
        // Quantize the cell to whole device pixels so cell rects land on
        // pixel boundaries and atlas tiles blit 1:1 (a fractional-pixel cell
        // would force resampling on every blit).
        let scale = window?.backingScaleFactor ?? 2.0
        cellSize = CGSize(width: max(1, (0.62 * size * scale).rounded()) / scale,
                          height: max(1, (1.05 * size * scale).rounded()) / scale)
        simulation.glyphCount = glyphs.count
        simulation.density = settings.density
        simulation.speed = settings.speed
        atlas = nil
        rebuildGrid()
        forceFullRedraw()
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
    /// then crops per-tile CGImages so each frame is pure blits. Rebuilt only
    /// when settings or backing scale change (atlas set to nil).
    private func buildAtlas() -> GlyphAtlas? {
        let scale = window?.backingScaleFactor ?? 2.0
        let levels = Self.trailLevels + 1
        // cellSize is quantized to device pixels, so these are exact.
        let tileW = max(1, Int((cellSize.width * scale).rounded()))
        let tileH = max(1, Int((cellSize.height * scale).rounded()))
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
