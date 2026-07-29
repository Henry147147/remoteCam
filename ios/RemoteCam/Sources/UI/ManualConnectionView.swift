import SwiftUI

struct ManualConnectionView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var hostname = ""
    @State private var port = ""
    let connect: (RemoteHost) -> Void

    var body: some View {
        NavigationStack {
            Form {
                Section("Windows computer") {
                    TextField("IP address or hostname", text: $hostname)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .keyboardType(.URL)
                    TextField("Port", text: $port)
                        .keyboardType(.numberPad)
                }
                Section {
                    Text("Use this when Bonjour discovery is blocked by a guest or enterprise network.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Connect manually")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Connect") {
                        guard let parsedPort = UInt16(port), parsedPort > 0 else { return }
                        let host = RemoteHost.manual(hostname: hostname, port: parsedPort)
                        dismiss()
                        connect(host)
                    }
                    .disabled(
                        hostname.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
                            || UInt16(port).map { $0 > 0 } != true
                    )
                }
            }
        }
    }
}
