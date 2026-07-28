import Foundation

@MainActor
final class AppModel: ObservableObject {
    let discovery = BonjourBrowser()
    let recentHosts = RecentHostsStore()

    @Published var connectionPhase: ConnectionPhase = .idle
    @Published var showingManualConnection = false

    func start() {
        discovery.start()
    }

    func connect(to host: RemoteHost) {
        connectionPhase = .connecting(host)
        // Network session integration lands with capture/encoder in the next slice.
    }

    func disconnect() {
        connectionPhase = .idle
    }
}
