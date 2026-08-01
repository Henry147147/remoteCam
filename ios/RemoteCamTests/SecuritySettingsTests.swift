import XCTest

@MainActor
final class SecuritySettingsTests: XCTestCase {
    private func isolatedDefaults(_ name: String) throws -> UserDefaults {
        let defaults = try XCTUnwrap(UserDefaults(suiteName: name))
        defaults.removePersistentDomain(forName: name)
        return defaults
    }

    func testDefaultsToAllowedWhenNeverSet() throws {
        let defaults = try isolatedDefaults("org.remotecam.tests.security.unset")
        let settings = SecuritySettings(defaults: defaults, key: "allowUnauthenticatedConnections")

        XCTAssertTrue(settings.allowsUnauthenticatedConnections)
    }

    // UserDefaults.bool(forKey:) returns false for a missing key, so a naive read would
    // make "the user turned this off" and "the user has never seen this" identical, and
    // the shipped default is on.
    func testRemembersAnExplicitOff() throws {
        let name = "org.remotecam.tests.security.off"
        let defaults = try isolatedDefaults(name)

        let settings = SecuritySettings(defaults: defaults, key: "allowUnauthenticatedConnections")
        settings.allowsUnauthenticatedConnections = false

        let reloaded = SecuritySettings(defaults: defaults, key: "allowUnauthenticatedConnections")
        XCTAssertFalse(reloaded.allowsUnauthenticatedConnections)
    }

    func testRemembersAnExplicitOn() throws {
        let name = "org.remotecam.tests.security.on"
        let defaults = try isolatedDefaults(name)

        let settings = SecuritySettings(defaults: defaults, key: "allowUnauthenticatedConnections")
        settings.allowsUnauthenticatedConnections = false
        settings.allowsUnauthenticatedConnections = true

        let reloaded = SecuritySettings(defaults: defaults, key: "allowUnauthenticatedConnections")
        XCTAssertTrue(reloaded.allowsUnauthenticatedConnections)
    }
}
