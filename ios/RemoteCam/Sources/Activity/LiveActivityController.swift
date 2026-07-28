@preconcurrency import ActivityKit
import Foundation

@MainActor
final class LiveActivityController {
    private var activity: Activity<RemoteCamActivityAttributes>?

    func start(host: RemoteHost, configuration: StreamConfiguration) async {
        if activity != nil {
            await update(host: host, configuration: configuration, status: "Live")
            return
        }
        guard ActivityAuthorizationInfo().areActivitiesEnabled else { return }
        let state = RemoteCamActivityAttributes.ContentState(
            hostName: host.name,
            resolution: "\(configuration.width)×\(configuration.height)",
            framesPerSecond: configuration.framesPerSecond,
            status: "Live"
        )
        do {
            activity = try Activity.request(
                attributes: RemoteCamActivityAttributes(startedAt: Date()),
                content: ActivityContent(state: state, staleDate: nil),
                pushType: nil
            )
        } catch {
            activity = nil
        }
    }

    func update(host: RemoteHost, configuration: StreamConfiguration, status: String) async {
        let content = ActivityContent(
            state: RemoteCamActivityAttributes.ContentState(
                hostName: host.name,
                resolution: "\(configuration.width)×\(configuration.height)",
                framesPerSecond: configuration.framesPerSecond,
                status: status
            ),
            staleDate: nil
        )
        await activity?.update(content)
    }

    func end() async {
        guard let activity else { return }
        await activity.end(nil, dismissalPolicy: .immediate)
        self.activity = nil
    }
}
