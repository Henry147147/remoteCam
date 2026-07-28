import SwiftUI

struct StreamView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        StreamContent(model: model, camera: model.camera)
    }
}

private struct StreamContent: View {
    @ObservedObject var model: AppModel
    @ObservedObject var camera: CameraController
    @State private var showingControls = false

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if camera.previewEnabled {
                CameraPreview(
                    session: camera.session,
                    enabled: camera.previewEnabled,
                    currentZoom: camera.controls.zoom,
                    minimumZoom: camera.controls.minimumZoom,
                    maximumZoom: camera.controls.maximumZoom,
                    focus: model.focus,
                    zoom: { model.applyCameraUpdate(CameraControlUpdate(zoom: $0)) }
                )
                .ignoresSafeArea()
            } else {
                ContentUnavailableView(
                    "Preview disabled",
                    systemImage: "eye.slash",
                    description: Text("The Windows computer disabled local preview to save battery. Video is still streaming.")
                )
                .foregroundStyle(.white)
            }

            VStack {
                statusBar
                Spacer()
                controlsBar
            }
            .padding()

            if let message = camera.interruptionMessage ?? camera.errorMessage ?? model.streamError {
                Text(message)
                    .font(.callout)
                    .multilineTextAlignment(.center)
                    .padding()
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 14))
                    .padding()
            }
        }
        .preferredColorScheme(.dark)
        .sheet(isPresented: $showingControls) {
            CameraControlsView(model: model, camera: camera)
                .presentationDetents([.medium, .large])
        }
    }

    private var statusBar: some View {
        HStack(spacing: 8) {
            Circle().fill(statusColor).frame(width: 9, height: 9)
            Text(statusText).font(.subheadline.weight(.semibold))
            Spacer()
            if case .streaming(_, let configuration) = model.connectionPhase {
                Text("\(configuration.width)×\(configuration.height) · \(configuration.framesPerSecond) fps")
                    .font(.caption.monospacedDigit())
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 9)
        .background(.ultraThinMaterial, in: Capsule())
        .accessibilityElement(children: .combine)
    }

    private var controlsBar: some View {
        HStack(spacing: 18) {
            Button {
                showingControls = true
            } label: {
                Label("Camera controls", systemImage: "slider.horizontal.3")
            }
            .buttonStyle(StreamButtonStyle())

            if camera.controls.torchAvailable {
                Button {
                    model.applyCameraUpdate(CameraControlUpdate(torchEnabled: !camera.controls.torchEnabled))
                } label: {
                    Label("Torch", systemImage: camera.controls.torchEnabled ? "flashlight.on.fill" : "flashlight.off.fill")
                }
                .buttonStyle(StreamButtonStyle(active: camera.controls.torchEnabled))
            }

            Button(role: .destructive) {
                model.disconnect()
            } label: {
                Label("Stop", systemImage: "stop.fill")
            }
            .buttonStyle(StreamButtonStyle(active: true, tint: .red))
        }
        .labelStyle(.iconOnly)
        .font(.title3)
    }

    private var statusColor: Color {
        switch model.connectionPhase {
        case .streaming: .green
        case .reconnecting: .orange
        default: .yellow
        }
    }

    private var statusText: String {
        switch model.connectionPhase {
        case .streaming(let host, _): "Live · \(host.name)"
        case .reconnecting(_, let attempt): "Reconnecting · attempt \(attempt)"
        default: "Preparing camera"
        }
    }
}

private struct StreamButtonStyle: ButtonStyle {
    var active = false
    var tint: Color = .accentColor

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .frame(width: 52, height: 52)
            .background(active ? tint : Color.white.opacity(0.18), in: Circle())
            .foregroundStyle(.white)
            .scaleEffect(configuration.isPressed ? 0.92 : 1)
    }
}
