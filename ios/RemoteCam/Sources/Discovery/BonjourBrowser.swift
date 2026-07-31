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
    private var generation = 0

    func start() {
        guard browser == nil else { return }
        RemoteCamLog.info("discovery", "starting Bonjour browse for _remotecam._tcp")
        generation += 1
        let currentGeneration = generation
        errorMessage = nil
        isSearching = true
        let parameters = NWParameters.tcp
        parameters.includePeerToPeer = true
        let browser = NWBrowser(for: .bonjour(type: "_remotecam._tcp", domain: nil), using: parameters)
        self.browser = browser

        browser.stateUpdateHandler = { [weak self] state in
            Task { @MainActor in
                guard let self, self.generation == currentGeneration else { return }
                self.handle(state)
            }
        }
        browser.browseResultsChangedHandler = { [weak self] results, _ in
            var parsedByID: [String: (RemoteHost, NWEndpoint)] = [:]
            for result in results {
                guard let host = Self.parse(result) else { continue }
                // The same PC can arrive through more than one active interface.
                // Stable TXT identity wins; keeping one endpoint also prevents
                // Dictionary(uniqueKeysWithValues:) from trapping on duplicates.
                if parsedByID[host.id] == nil {
                    parsedByID[host.id] = (host, result.endpoint)
                }
            }
            let parsed = Array(parsedByID.values)
            Task { @MainActor in
                guard let self, self.generation == currentGeneration else { return }
                self.endpointsByID = Dictionary(uniqueKeysWithValues: parsed.map { ($0.0.id, $0.1) })
                self.hosts = parsed.map(\.0).sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
                RemoteCamLog.info(
                    "discovery",
                    "results changed; valid_hosts=\(self.hosts.count), names=\(self.hosts.map(\.name).joined(separator: ","))"
                )
            }
        }
        browser.start(queue: queue)
    }

    func restart() {
        RemoteCamLog.info("discovery", "restart requested")
        stop(clearResults: true)
        start()
    }

    func stop(clearResults: Bool = false) {
        RemoteCamLog.info("discovery", "stopping browse; clear_results=\(clearResults)")
        generation += 1
        let oldBrowser = browser
        browser = nil
        oldBrowser?.cancel()
        isSearching = false
        if clearResults {
            hosts = []
            endpointsByID = [:]
        }
    }

    func endpoint(for host: RemoteHost) -> NWEndpoint? {
        endpointsByID[host.id]
    }

    private func handle(_ state: NWBrowser.State) {
        switch state {
        case .ready:
            RemoteCamLog.info("discovery", "browser ready")
            isSearching = true
            errorMessage = nil
        case .failed(let error):
            RemoteCamLog.error("discovery", "browser failed: \(error.localizedDescription)")
            errorMessage = error.localizedDescription
            stop(clearResults: true)
        case .cancelled:
            RemoteCamLog.info("discovery", "browser cancelled")
            isSearching = false
        case .waiting(let error):
            RemoteCamLog.info("discovery", "browser waiting: \(error.localizedDescription)")
            isSearching = true
            errorMessage = "Waiting for local network access: \(error.localizedDescription)"
        case .setup:
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

        return RemoteHost.discovered(serviceName: serviceName, txt: txt)
    }

    deinit {
        browser?.cancel()
    }
}
