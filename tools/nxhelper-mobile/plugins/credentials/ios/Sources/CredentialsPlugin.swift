import Foundation
import Security
import Tauri

struct StoreArgs: Decodable {
  let host: String
  let value: String?
}

class CredentialsPlugin: Plugin {
  private func query(_ host: String) -> [String: Any] {
    [
      kSecClass as String: kSecClassGenericPassword,
      kSecAttrService as String: "com.nxhelper.app.portal",
      kSecAttrAccount as String: host,
    ]
  }
  @objc public func load(_ invoke: Invoke) throws {
    let args = try invoke.parseArgs(StoreArgs.self)
    var request = query(args.host)
    request[kSecReturnData as String] = true
    var result: CFTypeRef?
    let status = SecItemCopyMatching(request as CFDictionary, &result)
    if status == errSecItemNotFound {
      invoke.resolve([String: String]())
      return
    }
    guard status == errSecSuccess, let data = result as? Data,
      let value = String(data: data, encoding: .utf8)
    else {
      invoke.reject("Credential storage failed: \(status)")
      return
    }
    invoke.resolve(["value": value])
  }
  @objc public func save(_ invoke: Invoke) throws {
    let args = try invoke.parseArgs(StoreArgs.self)
    let request = query(args.host)
    if let value = args.value {
      let attributes: [String: Any] = [
        kSecValueData as String: Data(value.utf8),
        kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
      ]
      var status = SecItemUpdate(request as CFDictionary, attributes as CFDictionary)
      if status == errSecItemNotFound {
        status = SecItemAdd(request.merging(attributes) { _, new in new } as CFDictionary, nil)
      }
      guard status == errSecSuccess else {
        invoke.reject("Credential storage failed: \(status)")
        return
      }
    } else {
      let status = SecItemDelete(request as CFDictionary)
      guard status == errSecSuccess || status == errSecItemNotFound else {
        invoke.reject("Credential storage failed: \(status)")
        return
      }
    }
    invoke.resolve([String: String]())
  }
}

@_cdecl("init_plugin_credentials")
func initPlugin() -> Plugin { CredentialsPlugin() }
