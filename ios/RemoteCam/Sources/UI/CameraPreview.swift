@preconcurrency import AVFoundation
import SwiftUI

struct CameraPreview: UIViewRepresentable {
    let session: AVCaptureSession
    let enabled: Bool
    let currentZoom: Double
    let minimumZoom: Double
    let maximumZoom: Double
    let focus: (CGPoint) -> Void
    let zoom: (Double) -> Void

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        view.previewLayer.session = session
        view.previewLayer.videoGravity = .resizeAspectFill
        view.onFocus = focus
        view.onZoom = zoom
        view.currentZoom = currentZoom
        view.minimumZoom = minimumZoom
        view.maximumZoom = maximumZoom
        return view
    }

    func updateUIView(_ view: PreviewView, context: Context) {
        view.previewLayer.session = enabled ? session : nil
        view.onFocus = focus
        view.onZoom = zoom
        view.currentZoom = currentZoom
        view.minimumZoom = minimumZoom
        view.maximumZoom = maximumZoom
    }
}

final class PreviewView: UIView {
    var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    var onFocus: ((CGPoint) -> Void)?
    var onZoom: ((Double) -> Void)?
    var currentZoom = 1.0
    var minimumZoom = 1.0
    var maximumZoom = 1.0
    private var pinchStartZoom = 1.0

    override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }

    override init(frame: CGRect) {
        super.init(frame: frame)
        addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(tapped(_:))))
        addGestureRecognizer(UIPinchGestureRecognizer(target: self, action: #selector(pinched(_:))))
        isAccessibilityElement = true
        accessibilityLabel = "Camera preview"
        accessibilityHint = "Double tap to focus the camera"
    }

    required init?(coder: NSCoder) {
        nil
    }

    @objc private func tapped(_ recognizer: UITapGestureRecognizer) {
        let devicePoint = previewLayer.captureDevicePointConverted(fromLayerPoint: recognizer.location(in: self))
        onFocus?(devicePoint)
    }

    @objc private func pinched(_ recognizer: UIPinchGestureRecognizer) {
        if recognizer.state == .began { pinchStartZoom = currentZoom }
        guard recognizer.state == .began || recognizer.state == .changed else { return }
        let requested = pinchStartZoom * Double(recognizer.scale)
        onZoom?(min(max(requested, minimumZoom), maximumZoom))
    }
}
