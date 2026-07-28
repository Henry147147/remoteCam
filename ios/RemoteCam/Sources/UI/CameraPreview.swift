@preconcurrency import AVFoundation
import SwiftUI

struct CameraPreview: UIViewRepresentable {
    let session: AVCaptureSession
    let enabled: Bool
    let focus: (CGPoint) -> Void

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        view.previewLayer.session = session
        view.previewLayer.videoGravity = .resizeAspectFill
        view.onFocus = focus
        return view
    }

    func updateUIView(_ view: PreviewView, context: Context) {
        view.previewLayer.session = enabled ? session : nil
        view.onFocus = focus
    }
}

final class PreviewView: UIView {
    var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    var onFocus: ((CGPoint) -> Void)?

    override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }

    override init(frame: CGRect) {
        super.init(frame: frame)
        addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(tapped(_:))))
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
}
