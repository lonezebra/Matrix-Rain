import Foundation

/// Display-independent Matrix rain simulation (SPEC.md section 1).
/// Pure Swift + Foundation only — no AppKit — so it stays testable.
final class RainSimulation {

    static let katakana = "ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝ"
    static let fullGlyphSet: [Character] = Array(katakana + "0123456789" + "Z:・.\"=*+-<>¦｜")
    /// Used when no available font covers the katakana range.
    static let fallbackGlyphSet: [Character] = Array("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ")

    /// Base fall rate in rows/second; multiplied by the user speed setting
    /// and a per-stream random factor.
    static let baseSpeed = 10.0
    /// 1.5 mutations per cell per minute.
    static let mutationRate = 1.5 / 60.0

    private struct Stream {
        var headRow: Double
        var speedFactor: Double   // uniform(0.6, 1.4)
        var length: Double        // trail length in cells
        var lastPaintedRow: Int   // last integer row the head has painted
    }

    private(set) var cols = 0
    private(set) var rows = 0

    /// Trail brightness per cell (row-major: `row * cols + col`), 0.0 – 1.0.
    private(set) var brightness: [Float] = []
    /// Per-cell index into the active glyph set.
    private(set) var glyph: [Int] = []

    /// Fraction of columns targeted to be active (0.05 – 1.0).
    var density = 0.75
    /// Multiplier on the base fall rate (0.5 – 3.0).
    var speed = 1.0

    /// Size of the glyph set indices are drawn from.
    var glyphCount: Int {
        didSet {
            glyphCount = max(1, glyphCount)
            if glyphCount != oldValue {
                for i in glyph.indices { glyph[i] %= glyphCount }
            }
        }
    }

    /// Brightness decay per second per cell, fixed when the head paints it.
    private var fade: [Float] = []
    /// At most one active stream per column.
    private var streams: [Stream?] = []
    private var activeStreams = 0

    init(cols: Int, rows: Int, glyphCount: Int) {
        self.glyphCount = max(1, glyphCount)
        resize(cols: cols, rows: rows)
    }

    /// Rebuilds the grid, dropping all streams (called on size/layout changes).
    func resize(cols: Int, rows: Int) {
        self.cols = max(1, cols)
        self.rows = max(1, rows)
        let count = self.cols * self.rows
        brightness = Array(repeating: 0, count: count)
        fade = Array(repeating: 0, count: count)
        glyph = (0..<count).map { _ in Int.random(in: 0..<glyphCount) }
        streams = Array(repeating: nil, count: self.cols)
        activeStreams = 0
    }

    /// Integer row of the bright head glyph in `column`, or nil when the head
    /// is off-screen (or the column is idle).
    func headRow(inColumn column: Int) -> Int? {
        guard let s = streams[column] else { return nil }
        let r = Int(s.headRow.rounded(.down))
        return r >= 0 && r < rows ? r : nil
    }

    /// Advances the simulation by `dt` seconds (monotonic-clock delta).
    func update(dt: Double) {
        guard dt > 0 else { return }

        // 1–2. Advance heads; every newly entered cell gets full brightness
        // and a fresh random glyph (so the head changes character per row).
        for c in 0..<cols {
            guard var s = streams[c] else { continue }
            let rowsPerSecond = Self.baseSpeed * speed * s.speedFactor
            s.headRow += rowsPerSecond * dt
            let newRow = Int(s.headRow.rounded(.down))
            if newRow > s.lastPaintedRow {
                // Brightness fades 1 -> 0 over `length` cells at `rowsPerSecond`.
                let cellFade = Float(rowsPerSecond / s.length)
                for r in (s.lastPaintedRow + 1)...newRow where r >= 0 && r < rows {
                    let i = r * cols + c
                    brightness[i] = 1.0
                    fade[i] = cellFade
                    glyph[i] = Int.random(in: 0..<glyphCount)
                }
                s.lastPaintedRow = newRow
            }
            if s.headRow - s.length > Double(rows) {
                streams[c] = nil
                activeStreams -= 1
            } else {
                streams[c] = s
            }
        }

        // 3. Decay trail brightness.
        let fdt = Float(dt)
        for i in brightness.indices where brightness[i] > 0 {
            brightness[i] = max(0, brightness[i] - fade[i] * fdt)
        }

        // 5. In-place glyph mutation while a cell is still visible.
        let mutationChance = Self.mutationRate * dt
        for i in glyph.indices where brightness[i] > 0 {
            if Double.random(in: 0..<1) < mutationChance {
                glyph[i] = Int.random(in: 0..<glyphCount)
            }
        }

        // 4 + density. Spawn into a random idle column.
        if Double(activeStreams) < density * Double(cols) {
            let spawnRate = 3.0 * Double(cols) * density / Double(rows)
            if Double.random(in: 0..<1) < 1 - exp(-dt * spawnRate) {
                spawnStream()
            }
        }
    }

    // MARK: - Display diffing

    /// Packs a quantized shade level and glyph index into a single cell state
    /// value: `(level << 16) | glyphIndex`. Level `levels` (one past the last
    /// trail bucket) is the head. A dark cell is represented as -1, not packed.
    static func packedState(level: Int, glyphIndex: Int) -> Int32 {
        Int32((level << 16) | glyphIndex)
    }

    /// Diffs the current quantized display state against `state`, the
    /// caller's last-drawn snapshot: rewrites `state` in place and appends the
    /// flat index (`row * cols + col`) of every cell whose quantized shade
    /// bucket or glyph changed to `dirtyCells`. Cells whose float brightness
    /// moved without crossing a bucket boundary are NOT reported — that is
    /// what makes per-frame invalidation cheap.
    ///
    /// `levels` is the number of trail buckets (the display's atlas depth);
    /// quantization matches rendering: head cells get level `levels`, lit
    /// trail cells get `min(levels - 1, Int(brightness * levels))`, dark
    /// cells are -1.
    ///
    /// Indices are appended column by column with rows ascending, so callers
    /// can coalesce contiguous vertical runs in one linear scan.
    /// If `state` does not match the grid size it is reset to all-dark first
    /// (every lit cell then reports dirty; callers treat that as a full
    /// redraw). Lives here, not in the view, so it is testable without AppKit.
    func syncDisplayState(levels: Int, state: inout [Int32], dirtyCells: inout [Int]) {
        let count = cols * rows
        if state.count != count {
            state = Array(repeating: -1, count: count)
        }
        let flevels = Float(levels)
        for c in 0..<cols {
            let head = headRow(inColumn: c)
            for r in 0..<rows {
                let i = r * cols + c
                let packed: Int32
                if r == head {
                    packed = Self.packedState(level: levels, glyphIndex: glyph[i])
                } else {
                    let b = brightness[i]
                    if b <= 0 {
                        packed = -1
                    } else {
                        let level = min(levels - 1, Int(b * flevels))
                        packed = Self.packedState(level: level, glyphIndex: glyph[i])
                    }
                }
                if state[i] != packed {
                    state[i] = packed
                    dirtyCells.append(i)
                }
            }
        }
    }

    private func spawnStream() {
        let idle = (0..<cols).filter { streams[$0] == nil }
        guard let c = idle.randomElement() else { return }
        // Start above the top edge so streams enter staggered.
        let start = -Double.random(in: 0..<Double(rows))
        streams[c] = Stream(headRow: start,
                            speedFactor: .random(in: 0.6...1.4),
                            length: Double.random(in: 0.35...0.95) * Double(rows),
                            lastPaintedRow: Int(start.rounded(.down)))
        activeStreams += 1
    }
}
