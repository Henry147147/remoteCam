import SwiftUI

struct RootView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        NavigationStack {
            List {
                statusSection
                discoveredSection
                recentSection
            }
            .navigationTitle("RemoteCam")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        model.showingManualConnection = true
                    } label: {
                        Label("Connect manually", systemImage: "plus")
                    }
                }
            }
            .sheet(isPresented: $model.showingManualConnection) {
                ManualConnectionView { host in
                    model.connect(to: host)
                }
            }
            .task { model.start() }
        }
    }

    private var statusSection: some View {
        Section {
            HStack(spacing: 12) {
                Image(systemName: statusIcon)
                    .font(.title2)
                    .foregroundStyle(statusColor)
                    .frame(width: 32)
                VStack(alignment: .leading, spacing: 2) {
                    Text(statusTitle).font(.headline)
                    Text(statusDetail).font(.subheadline).foregroundStyle(.secondary)
                }
            }
            .accessibilityElement(children: .combine)
        }
    }

    @ViewBuilder
    private var discoveredSection: some View {
        Section("Nearby computers") {
            if model.discovery.hosts.isEmpty {
                HStack {
                    if model.discovery.isSearching { ProgressView() }
                    Text(model.discovery.errorMessage ?? "Searching your local network…")
                        .foregroundStyle(.secondary)
                }
            } else {
                ForEach(model.discovery.hosts) { host in
                    HostRow(host: host) { model.connect(to: host) }
                }
            }
        }
    }

    @ViewBuilder
    private var recentSection: some View {
        if !model.recentHosts.hosts.isEmpty {
            Section("Recent") {
                ForEach(model.recentHosts.hosts) { host in
                    HostRow(host: host) { model.connect(to: host) }
                }
                .onDelete { indexes in
                    indexes.map { model.recentHosts.hosts[$0] }.forEach(model.recentHosts.remove)
                }
            }
        }
    }

    private var statusIcon: String {
        switch model.connectionPhase {
        case .idle: "iphone.gen3.radiowaves.left.and.right"
        case .failed: "exclamationmark.triangle.fill"
        default: "personalhotspot"
        }
    }

    private var statusColor: Color {
        switch model.connectionPhase {
        case .failed: .red
        case .streaming: .green
        default: .accentColor
        }
    }

    private var statusTitle: String {
        switch model.connectionPhase {
        case .idle: "Ready to connect"
        case .connecting: "Connecting"
        case .awaitingPairing: "Pairing required"
        case .ready: "Connected"
        case .streaming: "Camera is live"
        case .reconnecting: "Reconnecting"
        case .failed: "Connection failed"
        }
    }

    private var statusDetail: String {
        switch model.connectionPhase {
        case .idle: "Choose a Windows computer below."
        case .connecting(let host): "Opening \(host.name)…"
        case .awaitingPairing(let host): "Enter the code shown on \(host.name)."
        case .ready(let host, _): "Ready to stream to \(host.name)."
        case .streaming(let host, let configuration):
            "\(configuration.width)×\(configuration.height) at \(configuration.framesPerSecond) fps · \(host.name)"
        case .reconnecting(let host, let attempt): "Trying \(host.name) again (\(attempt))…"
        case .failed(let message): message
        }
    }
}

private struct HostRow: View {
    let host: RemoteHost
    let connect: () -> Void

    var body: some View {
        Button(action: connect) {
            HStack {
                Image(systemName: "pc")
                    .foregroundStyle(.tint)
                    .frame(width: 28)
                VStack(alignment: .leading) {
                    Text(host.name).foregroundStyle(.primary)
                    if !host.capabilities.isEmpty {
                        Text(host.capabilities.sorted().joined(separator: " · "))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                Spacer()
                Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary)
            }
        }
    }
}
