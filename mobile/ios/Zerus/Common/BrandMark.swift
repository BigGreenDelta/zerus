import SwiftUI

/// The Swarm mark from tray/resources/icons/hgs-zerus-symbolic.svg, drawn as three
/// rotated blades with the same transforms as the app icon.
struct BrandMark: Shape {
    func path(in rect: CGRect) -> Path {
        // Bounds of the transformed blades inside the 108-unit icon canvas.
        let content = CGRect(x: 21.3, y: 22.8, width: 63.2, height: 61.6)
        let scale = min(rect.width / content.width, rect.height / content.height)
        let fit = CGAffineTransform(translationX: -content.midX, y: -content.midY)
            .concatenating(CGAffineTransform(scaleX: scale, y: scale))
            .concatenating(CGAffineTransform(translationX: rect.midX, y: rect.midY))
        var result = Path()
        let outer = Self.group(pivot: CGPoint(x: 50, y: 50), scale: 0.8162, translate: CGPoint(x: 4, y: 4))
        for rotation in [0.0, 120, 240] {
            let transform = Self.group(pivot: CGPoint(x: 56, y: 30), scale: 0.88)
                .concatenating(Self.group(pivot: CGPoint(x: 50, y: 50), rotation: rotation))
                .concatenating(outer)
                .concatenating(fit)
            result.addPath(Self.blade.applying(transform))
        }
        return result
    }

    private static func group(pivot: CGPoint, rotation: CGFloat = 0, scale: CGFloat = 1, translate: CGPoint = .zero) -> CGAffineTransform {
        CGAffineTransform(translationX: -pivot.x, y: -pivot.y)
            .concatenating(CGAffineTransform(scaleX: scale, y: scale))
            .concatenating(CGAffineTransform(rotationAngle: rotation * .pi / 180))
            .concatenating(CGAffineTransform(translationX: translate.x + pivot.x, y: translate.y + pivot.y))
    }

    private static let blade: Path = {
        var path = Path()
        path.move(to: CGPoint(x: 39, y: 9))
        let curves: [(CGFloat, CGFloat, CGFloat, CGFloat, CGFloat, CGFloat)] = [
            (54, 3, 70, 13, 77, 29), (72, 25, 69, 24, 72, 32), (81, 44, 77, 58, 65, 65), (70, 51, 65, 36, 52, 28),
            (40, 21, 30, 25, 22, 34), (25, 21, 38, 15, 51, 19), (57, 21, 56, 16, 51, 13), (47, 11, 43, 10, 39, 9),
        ]
        for c in curves {
            path.addCurve(to: CGPoint(x: c.4, y: c.5), control1: CGPoint(x: c.0, y: c.1), control2: CGPoint(x: c.2, y: c.3))
        }
        path.closeSubpath()
        return path
    }()
}
