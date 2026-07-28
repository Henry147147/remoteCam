@preconcurrency import Network
import Foundation

@MainActor
final class BonjourBrowser: ObservableObject {
    @Published private(set) var hosts: [RemoteHost] = []
    @Published private(set) var isSearching = false
    @Published private(set) var errorMessage: String?

    private var browser: NWBrowser?
    private var endpointsByID: [String: NWEndpoint] = [:]
    private let queue = DispatchQueue(label: "org.remotecam.discovery")

    func start() {
        guard browser == nil else { return }
        let parameters = NWParameters.tcp
        parameters.includePeerToPeer = true
        let browser = NWBrowser(for: .bonjour(type: "_remotecam._tcp", domain: nil), using: parameters)
        self.browser = browser

        browser.stateUpdateHandler = { [weak self] state in
            Task { @MainActor in self?.handle(state) }
        }
        browser.browseResultsChangedHandler = { [weak self] results, _ in
            let parsed = results.compactMap { result in Self.parse(result).map { ($0, result.endpoint) } }
            Task { @MainActor in
                self?.endpointsByID = Dictionary(uniqueKeysWithValues: parsed.map { ($0.0.id, $0.1) })
                self?.hosts = parsed.map(\.0).sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
            }
        }
        isSearching = true
        browser.start(queue: queue)
    }

    func stop() {
        browser?.cancel()
        browser = nil
        isSearching = false
    }

    func endpoint(for host: RemoteHost) -> NWEndpoint? {
        endpointsByID[host.id]
    }

    private func handle(_ state: NWBrowser.State) {
        switch state {
        case .ready:
            isSearching = true
            errorMessage = nil
        case .failed(let error):
            errorMessage = error.localizedDescription
            stop()
        case .cancelled:
            isSearching = false
        case .setup, .waiting:
            break
        @unknown default:
            break
        }
    }

    nonisolated private static func parse(_ result: NWBrowser.Result) -> RemoteHost? {
        guard case .service(let serviceName, _, _, _) = result.endpoint else { return nil }

        var txt: [String: String] = [:]
        if case .bonjour(let metadata) = result.metadata {
            txt = metadata.dictionary
        }

        guard txt["v"] == "1", let stableID = txt["id"], stableID.count == 16 else { return nil }
        let caps = Set((txt["caps"] ?? "").split(separator: ",").map(String.init))
        return RemoteHost(
            id: stableID,
            name: txt["name"] ?? serviceName,
            hostname: serviceName,
            port: 0,
            capabilities: caps,
            source: .bonjour
        )
    }

    deinit {
        browser?.cancel()
    }
}
