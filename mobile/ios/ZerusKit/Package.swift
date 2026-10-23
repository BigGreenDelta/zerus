// swift-tools-version: 6.0
import PackageDescription

// Platform-neutral core of the iOS client: protocol models, policies ported from
// the Android client, the relay client and sealed persistence. `swift test` runs
// on a macOS host; the app target links the same library for iOS.
let package = Package(
    name: "ZerusKit",
    platforms: [.iOS(.v18), .macOS(.v15)],
    products: [.library(name: "ZerusKit", targets: ["ZerusKit"])],
    targets: [
        .target(name: "ZerusKit", linkerSettings: [.linkedLibrary("sqlite3")]),
        .testTarget(name: "ZerusKitTests", dependencies: ["ZerusKit"]),
    ]
)
