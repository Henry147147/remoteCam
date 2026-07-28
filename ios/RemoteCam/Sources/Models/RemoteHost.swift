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
