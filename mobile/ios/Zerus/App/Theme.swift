import SwiftUI

/// The shared dark palette of the desktop and Android clients.
enum Theme {
    static let mint = Color(hex: 0x67E8CB)
    static let amber = Color(hex: 0xFFCB7D)
    static let background = Color(hex: 0x101517)
    static let surface = Color(hex: 0x1A2225)
    static let muted = Color(hex: 0x9AACB2)
    /// Desktop's notice violet: messages the person did not write.
    static let notice = Color(hex: 0xC5A8F5)
    static let onSurface = Color(hex: 0xE9F0F2)
    static let outline = Color(hex: 0x354449)
    static let errorSurface = Color(hex: 0x3A2821)
    static let surfaceVariant = Color(hex: 0x26323A)
    static let selection = Color(hex: 0x243136)
}

extension Color {
    init(hex: UInt32, opacity: Double = 1) {
        self.init(.sRGB, red: Double((hex >> 16) & 0xFF) / 255, green: Double((hex >> 8) & 0xFF) / 255,
                  blue: Double(hex & 0xFF) / 255, opacity: opacity)
    }
}

/// A small capsule label, as Android's `Tag`.
struct Tag: View {
    let text: String
    var color: Color = Theme.mint

    var body: some View {
        Text(text)
            .font(.caption2.weight(.semibold))
            .foregroundStyle(color)
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(color.opacity(0.12), in: RoundedRectangle(cornerRadius: 6))
            .fixedSize()
    }
}

extension View {
    /// The ink background behind lists and screens, as on desktop and Android.
    func zerusScreen() -> some View {
        scrollContentBackground(.hidden)
            .background(Theme.background.ignoresSafeArea())
    }
}

extension ToolbarContent {
    /// Plain toolbar content such as the brand mark, without the iOS 26 glass capsule.
    @ToolbarContentBuilder
    func withoutGlassBackground() -> some ToolbarContent {
        if #available(iOS 26, *) {
            sharedBackgroundVisibility(.hidden)
        } else {
            self
        }
    }
}
