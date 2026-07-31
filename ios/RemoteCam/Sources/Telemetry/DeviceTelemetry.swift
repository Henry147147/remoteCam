@preconcurrency import CoreMotion
import Foundation
import UIKit

struct DeviceTelemetrySnapshot: Equatable, Sendable {
    enum Thermal: String, Sendable {
        case nominal
        case fair
        case serious
        case critical
    }

    var orientationDegrees: Double = 0
    var thermal: Thermal = .nominal
    var batteryLevel: Double = 0
    var charging = false
}

@MainActor
final class DeviceTelemetry: ObservableObject {
    @Published private(set) var snapshot = DeviceTelemetrySnapshot()
    var onUpdate: ((DeviceTelemetrySnapshot) -> Void)?

    private let motion = CMMotionManager()
    private var observers: [NSObjectProtocol] = []

    init() {
        RemoteCamLog.info("telemetry", "starting battery, thermal, orientation monitoring")
        UIDevice.current.isBatteryMonitoringEnabled = true
        UIDevice.current.beginGeneratingDeviceOrientationNotifications()
        observeNotifications()
        refreshSystemValues()
        startMotion()
    }

    private func observeNotifications() {
        let center = NotificationCenter.default
        let names: [Notification.Name] = [
            UIDevice.orientationDidChangeNotification,
            UIDevice.batteryLevelDidChangeNotification,
            UIDevice.batteryStateDidChangeNotification,
            ProcessInfo.thermalStateDidChangeNotification
        ]
        observers = names.map { name in
            center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                Task { @MainActor in self?.refreshSystemValues() }
            }
        }
    }

    private func startMotion() {
        guard motion.isDeviceMotionAvailable else {
            RemoteCamLog.error("telemetry", "device motion unavailable")
            return
        }
        motion.deviceMotionUpdateInterval = 0.2
        motion.startDeviceMotionUpdates(to: .main) { [weak self] motion, _ in
            guard let self, let roll = motion?.attitude.roll else { return }
            let degrees = roll * 180 / .pi
            Task { @MainActor in self.applyMotionDegrees(degrees) }
        }
    }

    private func applyMotionDegrees(_ rawDegrees: Double) {
        let deviceOrientation = UIDevice.current.orientation
        let degrees: Double
        switch deviceOrientation {
        case .portrait: degrees = 0
        case .landscapeLeft: degrees = 90
        case .portraitUpsideDown: degrees = 180
        case .landscapeRight: degrees = -90
        default: degrees = rawDegrees
        }
        guard degrees.isFinite, abs(snapshot.orientationDegrees - degrees) >= 1 else { return }
        snapshot.orientationDegrees = degrees
        onUpdate?(snapshot)
    }

    private func refreshSystemValues() {
        let previousThermal = snapshot.thermal
        let device = UIDevice.current
        snapshot.batteryLevel = min(max(Double(device.batteryLevel), 0), 1)
        snapshot.charging = device.batteryState == .charging || device.batteryState == .full
        snapshot.thermal = switch ProcessInfo.processInfo.thermalState {
        case .fair: .fair
        case .serious: .serious
        case .critical: .critical
        default: .nominal
        }
        if snapshot.thermal != previousThermal {
            RemoteCamLog.info("telemetry", "thermal state=\(snapshot.thermal.rawValue)")
        }
        onUpdate?(snapshot)
    }

    deinit {
        motion.stopDeviceMotionUpdates()
        observers.forEach(NotificationCenter.default.removeObserver)
    }
}
