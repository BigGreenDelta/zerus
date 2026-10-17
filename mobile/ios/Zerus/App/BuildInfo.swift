import Foundation

/// The installed build identity. A local build never claims a CI sequence.
struct BuildInfo {
    let version: String
    let build: String
    let channel: String
    let revision: String

    static let current = BuildInfo(bundle: .main)

    init(bundle: Bundle) {
        func value(_ key: String) -> String { (bundle.object(forInfoDictionaryKey: key) as? String) ?? "" }
        version = value("CFBundleShortVersionString")
        build = value("CFBundleVersion")
        channel = value("ZerusBuildChannel")
        revision = value("ZerusSourceRevision")
    }

    init(version: String, build: String, channel: String, revision: String) {
        self.version = version
        self.build = build
        self.channel = channel
        self.revision = revision
    }

    /// `0.1.0-dev.42` for Dev, `0.1.0` for Stable, `0.1.0-local` for local builds.
    var displayVersion: String {
        switch channel {
        case "dev": "\(version)-dev.\(build)"
        case "stable": version
        default: "\(version)-local"
        }
    }

    /// Details shown under the version: the source revision, and the CI build for published builds.
    var detail: String {
        let source = revision.isEmpty || revision == "unknown" ? "unknown source" : revision
        switch channel {
        case "dev", "stable": return "Build \(build) · \(source)"
        default: return "Local build · \(source)"
        }
    }
}
