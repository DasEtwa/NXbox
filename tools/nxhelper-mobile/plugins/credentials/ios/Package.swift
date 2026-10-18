// swift-tools-version:5.9
import PackageDescription

let package = Package(
  name: "tauri-plugin-credentials", platforms: [.iOS(.v14)],
  products: [
    .library(name: "tauri-plugin-credentials", type: .static, targets: ["CredentialsPlugin"])
  ], dependencies: [.package(name: "Tauri", path: "../.tauri/tauri-api")],
  targets: [
    .target(name: "CredentialsPlugin", dependencies: [.byName(name: "Tauri")], path: "Sources")
  ])
