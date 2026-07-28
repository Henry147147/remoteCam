import SwiftUI

struct CameraControlsView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var camera: CameraController

    var body: some View {
        NavigationStack {
            Form {
                cameraSection
                zoomSection
                focusSection
                exposureSection
                whiteBalanceSection
            }
            .navigationTitle("Camera controls")
            .navigationBarTitleDisplayMode(.inline)
        }
    }

    private var cameraSection: some View {
        Section("Lens") {
            Picker("Camera", selection: deviceBinding) {
                ForEach(camera.cameras) { descriptor in
                    Text("\(descriptor.position.rawValue.capitalized) · \(descriptor.lens.rawValue)")
                        .tag(descriptor.id)
                }
            }
            Toggle("Stabilization", isOn: Binding(
                get: { camera.controls.stabilizationEnabled },
                set: { model.applyCameraUpdate(CameraControlUpdate(stabilizationEnabled: $0)) }
            ))
        }
    }

    private var zoomSection: some View {
        Section("Zoom") {
            LabeledContent("Magnification", value: camera.controls.zoom.formatted(.number.precision(.fractionLength(1))) + "×")
            Slider(value: Binding(
                get: { camera.controls.zoom },
                set: { model.applyCameraUpdate(CameraControlUpdate(zoom: $0)) }
            ), in: camera.controls.minimumZoom...max(camera.controls.maximumZoom, camera.controls.minimumZoom + 0.01))
        }
    }

    private var focusSection: some View {
        Section("Focus") {
            Picker("Mode", selection: Binding(
                get: { camera.controls.focusMode },
                set: { model.applyCameraUpdate(CameraControlUpdate(focusMode: $0)) }
            )) {
                Text("Auto").tag(FocusMode.auto)
                Text("Lock").tag(FocusMode.locked)
                Text("Manual").tag(FocusMode.manual)
            }
            .pickerStyle(.segmented)
            if camera.controls.focusMode == .manual {
                LabeledContent("Distance", value: camera.controls.focus.formatted(.percent.precision(.fractionLength(0))))
                Slider(value: Binding(
                    get: { camera.controls.focus },
                    set: { model.applyCameraUpdate(CameraControlUpdate(focusMode: .manual, focus: $0)) }
                ), in: 0...1)
            }
            Text("You can also tap anywhere in the preview to set focus and exposure.")
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
    }

    private var exposureSection: some View {
        Section("Exposure") {
            Picker("Mode", selection: Binding(
                get: { camera.controls.exposureMode },
                set: { model.applyCameraUpdate(CameraControlUpdate(exposureMode: $0)) }
            )) {
                Text("Auto").tag(ExposureMode.auto)
                Text("Lock").tag(ExposureMode.locked)
                Text("Manual").tag(ExposureMode.manual)
            }
            .pickerStyle(.segmented)

            LabeledContent("Compensation", value: camera.controls.exposureBias.formatted(.number.precision(.fractionLength(1))) + " EV")
            Slider(value: Binding(
                get: { camera.controls.exposureBias },
                set: { model.applyCameraUpdate(CameraControlUpdate(exposureBias: $0)) }
            ), in: camera.controls.minimumExposureBias...max(camera.controls.maximumExposureBias, camera.controls.minimumExposureBias + 0.01))

            if camera.controls.exposureMode == .manual {
                LabeledContent("ISO", value: Int(camera.controls.iso).formatted())
                Slider(value: Binding(
                    get: { camera.controls.iso },
                    set: { model.applyCameraUpdate(CameraControlUpdate(exposureMode: .manual, iso: $0)) }
                ), in: camera.controls.minimumISO...max(camera.controls.maximumISO, camera.controls.minimumISO + 1))

                LabeledContent("Shutter", value: shutterDescription)
                Slider(value: shutterBinding, in: shutterRange)
            }
        }
    }

    private var whiteBalanceSection: some View {
        Section("White balance") {
            Picker("Mode", selection: Binding(
                get: { camera.controls.whiteBalanceMode },
                set: { model.applyCameraUpdate(CameraControlUpdate(whiteBalanceMode: $0)) }
            )) {
                Text("Auto").tag(WhiteBalanceMode.auto)
                Text("Lock").tag(WhiteBalanceMode.locked)
                Text("Manual").tag(WhiteBalanceMode.manual)
            }
            .pickerStyle(.segmented)
            if camera.controls.whiteBalanceMode == .manual {
                LabeledContent("Temperature", value: "\(Int(camera.controls.whiteBalanceKelvin)) K")
                Slider(value: Binding(
                    get: { camera.controls.whiteBalanceKelvin },
                    set: { model.applyCameraUpdate(CameraControlUpdate(whiteBalanceMode: .manual, whiteBalanceKelvin: $0)) }
                ), in: 2_000...10_000, step: 50)
            }
        }
    }

    private var deviceBinding: Binding<String> {
        Binding(
            get: { camera.controls.deviceID ?? "" },
            set: { model.switchCamera(to: $0) }
        )
    }

    private var shutterRange: ClosedRange<Double> {
        let minimum = max(camera.controls.minimumExposureSeconds, 1.0 / 100_000)
        let maximum = max(camera.controls.maximumExposureSeconds, minimum * 2)
        return log2(minimum)...log2(maximum)
    }

    private var shutterBinding: Binding<Double> {
        Binding(
            get: { log2(max(camera.controls.exposureSeconds, 1.0 / 100_000)) },
            set: { model.applyCameraUpdate(CameraControlUpdate(exposureMode: .manual, exposureSeconds: pow(2, $0))) }
        )
    }

    private var shutterDescription: String {
        let seconds = camera.controls.exposureSeconds
        if seconds >= 1 { return seconds.formatted(.number.precision(.fractionLength(2))) + " s" }
        return "1/\(max(Int((1 / max(seconds, 0.000_001)).rounded()), 1)) s"
    }
}
