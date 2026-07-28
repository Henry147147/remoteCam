import XCTest

@MainActor
final class RecentHostsStoreTests: XCTestCase {
    func testRecordsMostRecentFirstAndDeduplicates() {
        let suite = "RecentHostsStoreTests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = RecentHostsStore(defaults: defaults)
        let first = RemoteHost.manual(hostname: "192.168.1.4", port: 7_890)
        let second = RemoteHost.manual(hostname: "studio-pc.local", port: 7_890)

        store.record(first, at: Date(timeIntervalSince1970: 1))
        store.record(second, at: Date(timeIntervalSince1970: 2))
        store.record(first, at: Date(timeIntervalSince1970: 3))

        XCTAssertEqual(store.hosts.map(\.id), [first.id, second.id])
        XCTAssertEqual(store.hosts.first?.source, .recent)
    }
}
