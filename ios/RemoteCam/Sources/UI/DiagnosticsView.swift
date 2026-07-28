import AVFoundation
import SwiftUI

struct DiagnosticsView: View {
    @EnvironmentObject private var model: AppModel
    @ObservedObject var camera: CameraController
    @ObservedObject var telemetry: DeviceTelemetry

    var body: some View {
        NavigationStack {
            List {
                Section("Session") {
                    LabeledContent("State", value: connectionDescription)
                    LabeledContent("Protocol", value: "v1")
                    if case .streaming(_, let configuration) = model.connectionPhase {
                        LabeledContent("Codec", value: configuration.codec.rawValue.uppercased())
                        LabeledContent("Format", value: "\(configuration.width)×\(configuration.height) @ \(configuration.framesPerSecond)")
                        LabeledContent("Bitrate", value: bitrate(configuration.bitrate))
                    }
                }

                Section("Camera") {
                    LabeledContent("Permission", value: permissionDescription)
                    LabeledContent("Capture running", value: camera.isRunning ? "Yes" : "No")
                    LabeledContent("Multitasking access", value: camera.session.isMultitaskingCameraAccessSupported ? "Supported" : "Unavailable")
                    LabeledContent("Detected lenses", value: camera.cameras.count.formatted())
                    if let message = camera.errorMessage { Text(message).foregroundStyle(.red) }
                    if camera.permission == .denied {
                        Button("Open Camera Settings") { openSettings() }
                    }
                }

                Section("Device") {
                    LabeledContent("Thermal", value: telemetry.snapshot.thermal.rawValue.capitalized)
                    LabeledContent("Battery", value: telemetry.snapshot.batteryLevel.formatted(.percent.precision(.fractionLength(0))))
                    LabeledContent("Charging", value: telemetry.snapshot.charging ? "Yes" : "No")
                    LabeledContent("Orientation", value: telemetry.snapshot.orientationDegrees.formatted(.number.precision(.fractionLength(0))) + "°")
                }

                Section("Build") {
                    LabeledContent("Version", value: appVersion)
                    LabeledContent("iOS", value: UIDevice.current.systemVersion)
                }
            }
            .navigationTitle("Diagnostics")
            .navigationBarTitleDisplayMode(.inline)
        }
    }

    private var connectionDescription: String {
        switch model.connectionPhase {
        case .idle: "Idle"
        case .connecting: "Connecting"
        case .awaitingPairing: "Pairing"
        case .ready: "Preparing"
        case .streaming: "Streaming"
        case .reconnecting(_, let attempt): "Reconnecting (\(attempt))"
        case .failed: "Failed"
        }
    }

    private var permissionDescription: String {
        switch camera.permission {
        case .unknown: "Not requested"
        case .denied: "Denied"
        case .granted: "Granted"
        }
    }

    private var appVersion: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "—"
        let build = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "—"
        return "\(version) (\(build))"
    }

    private func bitrate(_ bitsPerSecond: Int) -> String {
        Measurement(value: Double(bitsPerSecond), unit: UnitInformationStorage.bits)
            .formatted(.measurement(width: .abbreviated)) + "/s"
    }

    private func openSettings() {
        guard let url = URL(string: UIApplication.openSettingsURLString) else { return }
        UIApplication.shared.open(url)
    }
}
