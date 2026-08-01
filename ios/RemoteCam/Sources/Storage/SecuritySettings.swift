import Foundation

/// The user's half of the pairing opt-out. The Windows app has the same option and both
/// must be enabled before a session skips authentication, so this alone never weakens
/// anything: it only decides whether this iPhone is willing to ask.
@MainActor
final class SecuritySettings: ObservableObject {
    @Published var allowsUnauthenticatedConnections: Bool {
        didSet {
            guard allowsUnauthenticatedConnections != oldValue else { return }
            defaults.set(allowsUnauthenticatedConnections, forKey: key)
        }
    }

    private let defaults: UserDefaults
    private let key: String

    init(defaults: UserDefaults = .standard, key: String = "allowUnauthenticatedConnections") {
        self.defaults = defaults
        self.key = key
        // `bool(forKey:)` cannot distinguish "off" from "never set", and the shipped
        // default is on. Ask whether the key exists before reading it.
        if defaults.object(forKey: key) == nil {
            allowsUnauthenticatedConnections = true
        } else {
            allowsUnauthenticatedConnections = defaults.bool(forKey: key)
        }
    }
}
