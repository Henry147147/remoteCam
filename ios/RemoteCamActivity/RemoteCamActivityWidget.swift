import ActivityKit
import SwiftUI
import WidgetKit

@main
struct RemoteCamActivityWidgetBundle: WidgetBundle {
    var body: some Widget {
        RemoteCamActivityWidget()
    }
}

struct RemoteCamActivityWidget: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: RemoteCamActivityAttributes.self) { context in
            HStack(spacing: 14) {
                Image(systemName: "video.fill")
                    .font(.title2)
                    .foregroundStyle(.red)
                VStack(alignment: .leading, spacing: 3) {
                    Text("RemoteCam · \(context.state.status)").font(.headline)
                    Text(context.state.hostName).font(.subheadline)
                    Text("\(context.state.resolution) · \(context.state.framesPerSecond) fps")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }
            .padding()
            .activityBackgroundTint(Color(.secondarySystemBackground))
            .activitySystemActionForegroundColor(.primary)
        } dynamicIsland: { context in
            DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    Label("Live", systemImage: "video.fill").foregroundStyle(.red)
                }
                DynamicIslandExpandedRegion(.trailing) {
                    Text("\(context.state.framesPerSecond) fps").monospacedDigit()
                }
                DynamicIslandExpandedRegion(.bottom) {
                    Text("\(context.state.hostName) · \(context.state.resolution)")
                }
            } compactLeading: {
                Image(systemName: "video.fill").foregroundStyle(.red)
            } compactTrailing: {
                Text("\(context.state.framesPerSecond)").monospacedDigit()
            } minimal: {
                Image(systemName: "video.fill").foregroundStyle(.red)
            }
        }
    }
}
