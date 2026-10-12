// swift-tools-version: 5.9
import PackageDescription
let package = Package(name: "DesktopHubHelper", platforms: [.macOS(.v13)],
    products: [.executable(name: "DesktopHubHelper", targets: ["DesktopHubHelper"])],
    targets: [.executableTarget(name: "DesktopHubHelper", linkerSettings: [
        .linkedFramework("CoreAudio"), .linkedFramework("IOBluetooth"),
        .linkedFramework("CoreWLAN"), .linkedFramework("AppKit"),
        .linkedFramework("AVFoundation")])])
