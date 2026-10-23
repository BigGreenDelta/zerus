import SwiftUI
import ZerusKit

/// Pairing never starts by itself: a link or scan only fills the form.
struct PairingSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let request: PairingRequest
    @State private var server = ""
    @State private var code = ""
    @State private var prepared = false
    @State private var scanning = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Button { scanning = true } label: { Label("Scan QR code", systemImage: "qrcode.viewfinder") }
                        .disabled(model.busy)
                } footer: {
                    Text("Scan an invitation from your machine, or enter its code. Review the gateway before pairing.")
                }
                Section {
                    if request.fromLink {
                        Label("Review this invitation before pairing.", systemImage: "exclamationmark.shield")
                            .foregroundStyle(Theme.amber)
                    }
                    TextField("Gateway URL", text: $server, prompt: Text("https://zerus.example"))
                        .textContentType(.URL)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    SecureField("Invitation code", text: $code)
                        .textContentType(.oneTimeCode)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                } footer: {
                    Text(WorkspaceProtocol.encryptedInvitationCode(code) ? "Messages are encrypted between enrolled devices. The computer that executes a request can read its content." : "Legacy invitations trust the relay with relayed content. Use a new phone invitation from an enrolled computer for encrypted transport.")
                }
            }
            .navigationTitle("Pair a workspace")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }.disabled(model.busy)
                }
                ToolbarItem(placement: .confirmationAction) {
                    if model.busy {
                        ProgressView()
                    } else {
                        Button("Pair") {
                            Task {
                                if await model.pair(url: server, code: code) { dismiss() }
                            }
                        }
                        .disabled(!model.storageReady || code.trimmingCharacters(in: .whitespaces).isEmpty ||
                                  server.trimmingCharacters(in: .whitespaces).isEmpty)
                    }
                }
            }
            .interactiveDismissDisabled(model.busy)
            .fullScreenCover(isPresented: $scanning) {
                PairingScanner { invite in
                    server = invite.server
                    code = invite.code
                }
            }
            .onAppear {
                guard !prepared else { return }
                prepared = true
                server = request.server
                code = request.code
            }
        }
    }
}
