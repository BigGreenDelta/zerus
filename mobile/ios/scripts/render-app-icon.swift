// Renders the iOS app icon from the Swarm blade geometry of
// tray/resources/icons/hgs-zerus-symbolic.svg, using the same blade transforms
// as the Android adaptive icon (mobile/android/app/src/main/res/drawable/ic_launcher_foreground.xml).
//
// Usage: swift mobile/ios/scripts/render-app-icon.swift OUTPUT.png [SIZE]
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let blade = "M39 9 C54 3 70 13 77 29 C72 25 69 24 72 32 C81 44 77 58 65 65 C70 51 65 36 52 28 C40 21 30 25 22 34 C25 21 38 15 51 19 C57 21 56 16 51 13 C47 11 43 10 39 9 Z"

func group(pivot: CGPoint, rotation: CGFloat = 0, scale: CGFloat = 1, translate: CGPoint = .zero) -> CGAffineTransform {
    // Android vector groups: T(translate + pivot) · R · S · T(-pivot), applied to child points.
    CGAffineTransform(translationX: -pivot.x, y: -pivot.y)
        .concatenating(CGAffineTransform(scaleX: scale, y: scale))
        .concatenating(CGAffineTransform(rotationAngle: rotation * .pi / 180))
        .concatenating(CGAffineTransform(translationX: translate.x + pivot.x, y: translate.y + pivot.y))
}

func path(_ data: String, _ transform: CGAffineTransform) -> CGPath {
    // Commands are attached to their first number ("M39 9"); separate them.
    let spaced = data.replacingOccurrences(of: "([MCZ])", with: " $1 ", options: .regularExpression)
    let tokens = spaced.split(separator: " ")
    let path = CGMutablePath()
    var index = 0
    func point() -> CGPoint {
        defer { index += 2 }
        return CGPoint(x: Double(tokens[index])!, y: Double(tokens[index + 1])!).applying(transform)
    }
    while index < tokens.count {
        let command = tokens[index]
        index += 1
        switch command {
        case "M": path.move(to: point())
        case "C": let a = point(), b = point(), c = point(); path.addCurve(to: c, control1: a, control2: b)
        case "Z": path.closeSubpath()
        default: fatalError("Unsupported path command \(command)")
        }
    }
    return path
}

let arguments = CommandLine.arguments
guard arguments.count >= 2 else { fatalError("Usage: render-app-icon.swift OUTPUT.png [SIZE]") }
let size = arguments.count > 2 ? Int(arguments[2])! : 1024
// The 108-unit adaptive canvas; iOS shows the central 84 units.
let visible: CGFloat = 84
let unit = CGFloat(size) / visible
let canvas = CGAffineTransform(translationX: -(108 - visible) / 2, y: -(108 - visible) / 2)
    .concatenating(CGAffineTransform(scaleX: unit, y: unit))

let space = CGColorSpace(name: CGColorSpace.sRGB)!
let context = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                        bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
// CoreGraphics has a bottom-left origin; vector data uses a top-left origin.
context.translateBy(x: 0, y: CGFloat(size))
context.scaleBy(x: 1, y: -1)
context.setFillColor(CGColor(srgbRed: 0x10 / 255, green: 0x15 / 255, blue: 0x17 / 255, alpha: 1))
context.fill(CGRect(x: 0, y: 0, width: size, height: size))
context.setFillColor(CGColor(srgbRed: 0x67 / 255, green: 0xE8 / 255, blue: 0xCB / 255, alpha: 1))
let outer = group(pivot: CGPoint(x: 50, y: 50), scale: 0.8162, translate: CGPoint(x: 4, y: 4))
for rotation in [0, 120, 240] as [CGFloat] {
    let transform = group(pivot: CGPoint(x: 56, y: 30), scale: 0.88)
        .concatenating(group(pivot: CGPoint(x: 50, y: 50), rotation: rotation))
        .concatenating(outer)
        .concatenating(canvas)
    context.addPath(path(blade, transform))
    context.fillPath()
}
let image = context.makeImage()!
let url = URL(fileURLWithPath: arguments[1])
let destination = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
CGImageDestinationAddImage(destination, image, nil)
guard CGImageDestinationFinalize(destination) else { fatalError("Could not write \(url.path)") }
