// swift-tools-version: 6.0
import PackageDescription

// Platform-neutral core of the iOS client: protocol models, policies ported from
// the Android client, the relay client and sealed persistence. `swift test` runs
// on a macOS host; the app target links the same library for iOS.
//
// The project task replica is the Rust crate crates/replica-ffi: run
// mobile/ios/scripts/build-replica.sh first, which writes Replica/ (git ignores it).
let package = Package(
    name: "ZerusKit",
    platforms: [.iOS(.v18), .macOS(.v15)],
    products: [.library(name: "ZerusKit", targets: ["ZerusKit", "ZerusReplica"])],
    targets: [
        .binaryTarget(name: "ZerusReplicaFFI", path: "Replica/ZerusReplicaFFI.xcframework"),
        .target(name: "ZerusReplica", dependencies: ["ZerusReplicaFFI"], path: "Replica/Swift"),
        .target(name: "ZerusKit", dependencies: ["ZerusReplica"]),
        .testTarget(name: "ZerusKitTests", dependencies: ["ZerusKit", "ZerusReplica"]),
    ]
)
