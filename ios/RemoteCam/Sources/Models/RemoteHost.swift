import Foundation

struct RemoteHost: Identifiable, Codable, Hashable, Sendable {
    enum DiscoverySource: String, Codable, Sendable {
        case bonjour
        case manual
        case recent
    }

    let id: String
    var name: String
    var hostname: String
    var port: UInt16
    var capabilities: Set<String>
    var source: DiscoverySource
    var lastConnectedAt: Date?

    init(
        id: String,
        name: String,
        hostname: String,
        port: UInt16,
        capabilities: Set<String> = [],
        source: DiscoverySource,
        lastConnectedAt: Date? = nil
    ) {
        self.id = id
        self.name = name
        self.hostname = hostname
        self.port = port
        self.capabilities = capabilities
        self.source = source
        self.lastConnectedAt = lastConnectedAt
    }
}

extension RemoteHost {
    static func discovered(serviceName: String, txt: [String: String]) -> RemoteHost? {
        guard txt["v"] == "1",
              let stableID = txt["id"],
              stableID.utf8.count == 16,
              stableID.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) else {
            return nil
        }

        let advertisedName = txt["name"]?.trimmingCharacters(in: .whitespacesAndNewlines)
        let displayName = advertisedName.flatMap { $0.isEmpty ? nil : $0 } ?? serviceName
        let capabilities = Set(
            (txt["caps"] ?? "")
                .split(separator: ",")
                .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
                .filter { !$0.isEmpty }
        )
        return RemoteHost(
            id: stableID,
            name: displayName,
            hostname: serviceName,
            port: 0,
            capabilities: capabilities,
            source: .bonjour
        )
    }

    static func manual(hostname: String, port: UInt16) -> RemoteHost {
        let normalizedHost = hostname.trimmingCharacters(in: .whitespacesAndNewlines)
        return RemoteHost(
            id: "manual:\(normalizedHost.lowercased()):\(port)",
            name: normalizedHost,
            hostname: normalizedHost,
            port: port,
            source: .manual
        )
    }
}
