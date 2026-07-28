import Foundation

enum CameraPosition: String, CaseIterable, Codable, Sendable {
    case front
    case back
}

enum CameraLens: String, CaseIterable, Codable, Sendable {
    case ultraWide = "ultra-wide"
    case wide
    case telephoto = "tele"
    case trueDepth = "true-depth"
    case other
}

struct CameraDescriptor: Identifiable, Hashable, Sendable {
    let id: String
    let name: String
    let position: CameraPosition
    let lens: CameraLens
}

enum FocusMode: String, Codable, Sendable {
    case auto
    case locked
    case manual
}

enum ExposureMode: String, Codable, Sendable {
    case auto
    case locked
    case manual
}

enum WhiteBalanceMode: String, Codable, Sendable {
    case auto
    case locked
    case manual
}

struct CameraControlState: Equatable, Sendable {
    var deviceID: String?
    var position: CameraPosition = .back
    var lens: CameraLens = .wide
    var zoom: Double = 1
    var minimumZoom: Double = 1
    var maximumZoom: Double = 1
    var focusMode: FocusMode = .auto
    var focus: Double = 0.5
    var exposureMode: ExposureMode = .auto
    var iso: Double = 0
    var minimumISO: Double = 0
    var maximumISO: Double = 0
    var exposureSeconds: Double = 0
    var minimumExposureSeconds: Double = 0
    var maximumExposureSeconds: Double = 0
    var exposureBias: Double = 0
    var minimumExposureBias: Double = 0
    var maximumExposureBias: Double = 0
    var whiteBalanceMode: WhiteBalanceMode = .auto
    var whiteBalanceKelvin: Double = 5_000
    var torchEnabled = false
    var torchAvailable = false
}

struct CameraControlUpdate: Sendable {
    var zoom: Double?
    var focusMode: FocusMode?
    var focus: Double?
    var exposureMode: ExposureMode?
    var iso: Double?
    var exposureSeconds: Double?
    var exposureBias: Double?
    var whiteBalanceMode: WhiteBalanceMode?
    var whiteBalanceKelvin: Double?
    var torchEnabled: Bool?
}
