import XCTest

@MainActor
final class RecentHostsStoreTests: XCTestCase {
    func testCreatesDiscoveredHostFromValidBonjourMetadata() throws {
        let host = try XCTUnwrap(RemoteHost.discovered(
            serviceName: "Studio PC",
            txt: [
                "v": "1",
                "name": "Editing Rig",
                "id": "0123456789abcdef",
                "caps": "h264, hevc,,"
            ]
        ))

        XCTAssertEqual(host.id, "0123456789abcdef")
        XCTAssertEqual(host.name, "Editing Rig")
        XCTAssertEqual(host.capabilities, ["h264", "hevc"])
        XCTAssertEqual(host.source, .bonjour)
        XCTAssertEqual(host.port, 0)
    }

    func testRejectsIncompatibleOrMalformedBonjourMetadata() {
        XCTAssertNil(RemoteHost.discovered(
            serviceName: "PC",
            txt: ["v": "2", "id": "0123456789abcdef"]
        ))
        XCTAssertNil(RemoteHost.discovered(
            serviceName: "PC",
            txt: ["v": "1", "id": "0123456789ABCDEF"]
        ))
        XCTAssertNil(RemoteHost.discovered(
            serviceName: "PC",
            txt: ["v": "1", "id": "not-hex-not-valid"]
        ))
    }

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
