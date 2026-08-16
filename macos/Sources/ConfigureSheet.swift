import AppKit

/// Programmatically built configure sheet (no xib): preset popup, custom
/// color well, sliders for density/size/speed, OK/Cancel.
final class ConfigureSheetController: NSObject {

    let window: NSWindow

    private let settings: Settings
    private let onApply: () -> Void

    private let presetPopup: NSPopUpButton
    private let colorWell: NSColorWell
    private let densitySlider: NSSlider
    private let sizeSlider: NSSlider
    private let speedSlider: NSSlider
    private let densityValue: NSTextField
    private let sizeValue: NSTextField
    private let speedValue: NSTextField

    private static let customItemTitle = "Custom"

    init(settings: Settings, onApply: @escaping () -> Void) {
        self.settings = settings
        self.onApply = onApply

        // Stored objects are fully configured by static factories: mutating a
        // stored property's object before super.init would be a use of self.
        window = ConfigureSheetController.makeWindow()
        presetPopup = NSPopUpButton(frame: NSRect(x: 150, y: 242, width: 220, height: 25),
                                    pullsDown: false)
        colorWell = NSColorWell(frame: NSRect(x: 150, y: 206, width: 60, height: 24))
        densitySlider = ConfigureSheetController.makeSlider(y: 162, min: 0.05, max: 1.0)
        sizeSlider = ConfigureSheetController.makeSlider(y: 126, min: 8, max: 64)
        speedSlider = ConfigureSheetController.makeSlider(y: 90, min: 0.5, max: 3.0)
        densityValue = ConfigureSheetController.valueLabel(y: 164)
        sizeValue = ConfigureSheetController.valueLabel(y: 128)
        speedValue = ConfigureSheetController.valueLabel(y: 92)

        super.init()

        let content = window.contentView!
        content.addSubview(ConfigureSheetController.rowLabel("Preset:", y: 246))
        content.addSubview(ConfigureSheetController.rowLabel("Color:", y: 209))
        content.addSubview(ConfigureSheetController.rowLabel("Density:", y: 164))
        content.addSubview(ConfigureSheetController.rowLabel("Glyph size:", y: 128))
        content.addSubview(ConfigureSheetController.rowLabel("Speed:", y: 92))
        content.addSubview(presetPopup)
        content.addSubview(colorWell)
        content.addSubview(densitySlider)
        content.addSubview(sizeSlider)
        content.addSubview(speedSlider)
        content.addSubview(densityValue)
        content.addSubview(sizeValue)
        content.addSubview(speedValue)

        presetPopup.addItems(withTitles: Settings.presets.map { $0.name })
        presetPopup.menu?.addItem(.separator())
        presetPopup.addItem(withTitle: ConfigureSheetController.customItemTitle)
        presetPopup.target = self
        presetPopup.action = #selector(presetChanged(_:))

        colorWell.target = self
        colorWell.action = #selector(colorChanged(_:))

        for slider in [densitySlider, sizeSlider, speedSlider] {
            slider.target = self
            slider.action = #selector(sliderChanged(_:))
        }

        let cancelButton = NSButton(title: "Cancel", target: self, action: #selector(cancel(_:)))
        cancelButton.frame = NSRect(x: 258, y: 16, width: 92, height: 32)
        cancelButton.bezelStyle = .rounded
        cancelButton.keyEquivalent = "\u{1b}"
        content.addSubview(cancelButton)

        let okButton = NSButton(title: "OK", target: self, action: #selector(ok(_:)))
        okButton.frame = NSRect(x: 352, y: 16, width: 92, height: 32)
        okButton.bezelStyle = .rounded
        okButton.keyEquivalent = "\r"
        content.addSubview(okButton)

        reload()
    }

    /// Refreshes all controls from the persisted settings (called every time
    /// the sheet is about to be shown, so Cancel discards edits).
    func reload() {
        colorWell.color = settings.color
        densitySlider.doubleValue = settings.density
        sizeSlider.doubleValue = Double(settings.size)
        speedSlider.doubleValue = settings.speed
        selectPreset(matching: settings.colorHex)
        updateValueLabels()
    }

    // MARK: - Actions

    @objc private func presetChanged(_ sender: NSPopUpButton) {
        let index = sender.indexOfSelectedItem
        guard index >= 0 && index < Settings.presets.count,
              let color = NSColor(hexString: Settings.presets[index].hex)
        else { return }
        colorWell.color = color
    }

    @objc private func colorChanged(_ sender: NSColorWell) {
        selectPreset(matching: sender.color.srgbHexString)
    }

    @objc private func sliderChanged(_ sender: NSSlider) {
        updateValueLabels()
    }

    @objc private func ok(_ sender: Any?) {
        settings.color = colorWell.color
        settings.density = densitySlider.doubleValue
        settings.size = Int(sizeSlider.doubleValue.rounded())
        settings.speed = speedSlider.doubleValue
        settings.synchronize()
        onApply()
        dismiss(.OK)
    }

    @objc private func cancel(_ sender: Any?) {
        dismiss(.cancel)
    }

    private func dismiss(_ code: NSApplication.ModalResponse) {
        NSColorPanel.shared.close()
        if let parent = window.sheetParent {
            parent.endSheet(window, returnCode: code)
        } else {
            // Legacy ScreenSaverEngine path (pre-sheetParent API).
            NSApp.endSheet(window, returnCode: code.rawValue)
            window.orderOut(nil)
        }
    }

    // MARK: - Helpers

    private func selectPreset(matching hex: String) {
        let upper = hex.uppercased()
        if let index = Settings.presets.firstIndex(where: { $0.hex.uppercased() == upper }) {
            presetPopup.selectItem(at: index)
        } else {
            presetPopup.selectItem(withTitle: ConfigureSheetController.customItemTitle)
        }
    }

    private func updateValueLabels() {
        densityValue.stringValue = String(format: "%.2f", densitySlider.doubleValue)
        sizeValue.stringValue = "\(Int(sizeSlider.doubleValue.rounded())) pt"
        speedValue.stringValue = String(format: "%.2fx", speedSlider.doubleValue)
    }

    private static func makeWindow() -> NSWindow {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 460, height: 296),
                              styleMask: [.titled],
                              backing: .buffered,
                              defer: false)
        window.title = "Matrix Rain"
        window.isReleasedWhenClosed = false
        return window
    }

    private static func makeSlider(y: CGFloat, min: Double, max: Double) -> NSSlider {
        let slider = NSSlider(frame: NSRect(x: 150, y: y, width: 220, height: 21))
        slider.minValue = min
        slider.maxValue = max
        slider.isContinuous = true
        return slider
    }

    private static func rowLabel(_ text: String, y: CGFloat) -> NSTextField {
        let label = NSTextField(labelWithString: text)
        label.frame = NSRect(x: 20, y: y, width: 120, height: 18)
        label.alignment = .right
        return label
    }

    private static func valueLabel(y: CGFloat) -> NSTextField {
        let label = NSTextField(labelWithString: "")
        label.frame = NSRect(x: 378, y: y, width: 66, height: 18)
        label.alignment = .left
        return label
    }
}
