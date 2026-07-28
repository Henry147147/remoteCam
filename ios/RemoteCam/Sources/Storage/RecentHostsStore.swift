import Foundation

@MainActor
final class RecentHostsStore: ObservableObject {
    @Published private(set) var hosts: [RemoteHost] = []

    private let defaults: UserDefaults
    private let key: String
    private let encoder = JSONEncoder()
    private let decoder = JSONDecoder()

    init(defaults: UserDefaults = .standard, key: String = "recentHosts") {
        self.defaults = defaults
        self.key = key
        load()
    }

    func record(_ host: RemoteHost, at date: Date = Date()) {
        var recent = host
        recent.lastConnectedAt = date
        recent.source = .recent
        hosts.removeAll { $0.id == recent.id }
        hosts.insert(recent, at: 0)
        hosts = Array(hosts.prefix(10))
        persist()
    }

    func remove(_ host: RemoteHost) {
        hosts.removeAll { $0.id == host.id }
        persist()
    }

    func removeAll() {
        hosts.removeAll()
        persist()
    }

    private func load() {
        guard let data = defaults.data(forKey: key),
              let decoded = try? decoder.decode([RemoteHost].self, from: data) else {
            hosts = []
            return
        }
        hosts = decoded.sorted {
            ($0.lastConnectedAt ?? .distantPast) > ($1.lastConnectedAt ?? .distantPast)
        }
    }

    private func persist() {
        guard let data = try? encoder.encode(hosts) else { return }
        defaults.set(data, forKey: key)
    }
}
