import Foundation
import Security

enum DeviceIdentity {
    private static let service = "org.remotecam.identity"
    private static let account = "device-id"
    private static let fallbackDefaultsKey = "deviceIdentityFallback"

    static func loadOrCreate() -> String {
        if let existing = read(), isValid(existing) { return existing }
        if let fallback = UserDefaults.standard.string(forKey: fallbackDefaultsKey),
           isValid(fallback) {
            return fallback
        }
        let id = (0..<8).map { _ in String(format: "%02x", UInt8.random(in: .min ... .max)) }.joined()
        if store(id) {
            UserDefaults.standard.removeObject(forKey: fallbackDefaultsKey)
        } else {
            UserDefaults.standard.set(id, forKey: fallbackDefaultsKey)
        }
        return id
    }

    private static func isValid(_ value: String) -> Bool {
        value.utf8.count == 16 && value.utf8.allSatisfy {
            (48...57).contains($0) || (97...102).contains($0)
        }
    }

    private static func read() -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    @discardableResult
    private static func store(_ value: String) -> Bool {
        let base: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account
        ]
        SecItemDelete(base as CFDictionary)
        var item = base
        item[kSecValueData as String] = Data(value.utf8)
        item[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        return SecItemAdd(item as CFDictionary, nil) == errSecSuccess
    }
}
