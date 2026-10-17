import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers
import ZerusKit

enum PickKind: Equatable { case photos, camera, files }

/// A picker opened for one durable selection.
struct PickRequest: Equatable {
    let id: String
    let kind: PickKind
    let remaining: Int
}

/// A picked photo or video copied into a private temporary folder. Larger items are refused
/// before copying, and compatible encoding turns HEIC/HEVC into JPEG/H.264 for agents.
struct PickedFile: Transferable {
    let url: URL

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(importedContentType: .data) { received in
            let size = (try? FileManager.default.attributesOfItem(atPath: received.file.path)[.size] as? NSNumber)?.int64Value ?? 0
            if size > AttachmentPolicy.maxFileBytes { throw ZerusError.invalid("Each attachment must be at most 10 MiB.") }
            let folder = FileManager.default.temporaryDirectory.appendingPathComponent("picked-\(UUID().uuidString.lowercased())", isDirectory: true)
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            let copy = folder.appendingPathComponent(AttachmentPolicy.filename(received.file.lastPathComponent))
            try FileManager.default.copyItem(at: received.file, to: copy)
            return PickedFile(url: copy)
        }
    }
}

enum PickedTypes {
    static func mime(_ url: URL) -> String? { UTType(filenameExtension: url.pathExtension)?.preferredMIMEType }
}

/// Presents the photo library, camera or Files for a durable selection and hands the result to the model.
struct AttachmentPickers: ViewModifier {
    @Environment(AppModel.self) private var app
    @Binding var request: PickRequest?
    @State private var photos: [PhotosPickerItem] = []
    @State private var photosId = ""
    @State private var loadingPhotos = false

    private func presented(_ kind: PickKind) -> Binding<Bool> {
        Binding(get: { request?.kind == kind }, set: { shown in
            guard !shown, let current = request, current.kind == kind else { return }
            request = nil
            if kind == .photos {
                // The selection arrives just before or after dismissal; cancel only when none came.
                Task {
                    try? await Task.sleep(for: .milliseconds(500))
                    if !loadingPhotos && photosId == current.id { cancel(current.id) }
                }
            }
        })
    }

    func body(content: Content) -> some View {
        content
            .photosPicker(isPresented: presented(.photos), selection: $photos, maxSelectionCount: max(1, request?.remaining ?? 1),
                          matching: .any(of: [.images, .videos]), preferredItemEncoding: .compatible)
            .fileImporter(isPresented: presented(.files), allowedContentTypes: [.item], allowsMultipleSelection: true) { result in
                guard let id = app.conversation.messageState.pickerId.nonEmpty else { return }
                switch result {
                case .success(let urls):
                    app.conversation.finishAttachmentPick(urls.map { .file($0, name: $0.lastPathComponent, mime: PickedTypes.mime($0), scoped: true, temporary: false) },
                                                          selectionId: id)
                case .failure:
                    app.error = "The selected document could not be opened."
                    cancel(id)
                }
            } onCancellation: {
                if let id = app.conversation.messageState.pickerId.nonEmpty { cancel(id) }
            }
            .fullScreenCover(isPresented: presented(.camera)) {
                CameraPicker { data in
                    request = nil
                    guard let id = app.conversation.messageState.pickerId.nonEmpty else { return }
                    if let data {
                        app.conversation.finishAttachmentPick([.data(data, name: "Photo \(Self.stamp.string(from: Date())).jpg", mime: "image/jpeg")], selectionId: id)
                    } else {
                        cancel(id)
                    }
                }
                .ignoresSafeArea()
            }
            .onChange(of: request) { _, value in if value?.kind == .photos { photosId = value?.id ?? "" } }
            .onChange(of: photos) { _, items in
                guard !items.isEmpty else { return }
                let id = photosId
                photos = []
                loadingPhotos = true
                Task {
                    defer {
                        loadingPhotos = false
                        photosId = ""
                    }
                    var sources: [PickedSource] = []
                    do {
                        for item in items {
                            guard let picked = try await item.loadTransferable(type: PickedFile.self) else {
                                throw ZerusError.invalid("A selected item could not be loaded.")
                            }
                            sources.append(.file(picked.url, name: picked.url.lastPathComponent, mime: PickedTypes.mime(picked.url), scoped: false, temporary: true))
                        }
                    } catch {
                        ConversationModel.discardTemporary(sources)
                        app.error = (error as? ZerusError)?.message ?? "The selected photos could not be loaded. Your draft is preserved."
                        cancel(id)
                        return
                    }
                    app.conversation.finishAttachmentPick(sources, selectionId: id)
                }
            }
    }

    private func cancel(_ id: String) { app.conversation.cancelAttachmentPick(id) }

    static let stamp: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd 'at' HH.mm.ss"
        return formatter
    }()
}

/// Takes one photo; the image is encoded as JPEG and never stored in the photo library.
struct CameraPicker: UIViewControllerRepresentable {
    let finished: (Data?) -> Void

    static var available: Bool { UIImagePickerController.isSourceTypeAvailable(.camera) }

    func makeCoordinator() -> Coordinator { Coordinator(finished: finished) }

    func makeUIViewController(context: Context) -> UIImagePickerController {
        let picker = UIImagePickerController()
        picker.sourceType = .camera
        picker.mediaTypes = [UTType.image.identifier]
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ controller: UIImagePickerController, context: Context) {}

    final class Coordinator: NSObject, UIImagePickerControllerDelegate, UINavigationControllerDelegate {
        let finished: (Data?) -> Void
        private var done = false
        init(finished: @escaping (Data?) -> Void) { self.finished = finished }

        func imagePickerController(_ picker: UIImagePickerController, didFinishPickingMediaWithInfo info: [UIImagePickerController.InfoKey: Any]) {
            guard !done else { return }
            done = true
            let image = info[.originalImage] as? UIImage
            finished(image?.jpegData(compressionQuality: 0.85))
        }

        func imagePickerControllerDidCancel(_ picker: UIImagePickerController) {
            guard !done else { return }
            done = true
            finished(nil)
        }
    }
}
