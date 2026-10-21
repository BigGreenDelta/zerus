import Foundation
import Synchronization
@testable import ZerusKit

/// A synthetic gateway behind URLProtocol. Each test installs its own responder
/// and reads the recorded requests; no network access happens.
final class StubProtocol: URLProtocol, @unchecked Sendable {
    struct Recorded: Sendable {
        let method: String
        let url: URL
        let headers: [String: String]
        let body: Data
        var json: JSON? { body.isEmpty ? nil : try? JSON.parse(body) }
    }
    enum Reply: Sendable {
        case response(status: Int, body: Data, headers: [String: String] = [:])
        case failure(URLError.Code)
        /// Never answers; only cancellation ends the request, which `stopped` counts.
        case hang
    }
    typealias Responder = @Sendable (Recorded) -> Reply

    static let responder = Mutex<Responder?>(nil)
    static let recorded = Mutex<[Recorded]>([])
    static let stoppedHanging = Mutex(0)
    private let hanging = Mutex(false)

    static func reset(_ responder: @escaping Responder) {
        self.responder.withLock { $0 = responder }
        recorded.withLock { $0 = [] }
        stoppedHanging.withLock { $0 = 0 }
    }
    static var requests: [Recorded] { recorded.withLock { $0 } }

    static func json(_ status: Int = 200, _ text: String) -> Reply { .response(status: status, body: Data(text.utf8)) }

    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    override func startLoading() {
        var body = request.httpBody ?? Data()
        if body.isEmpty, let stream = request.httpBodyStream {
            stream.open()
            var buffer = [UInt8](repeating: 0, count: 65536)
            while stream.hasBytesAvailable {
                let count = stream.read(&buffer, maxLength: buffer.count)
                if count <= 0 { break }
                body.append(buffer, count: count)
            }
            stream.close()
        }
        let entry = Recorded(method: request.httpMethod ?? "GET", url: request.url!, headers: request.allHTTPHeaderFields ?? [:], body: body)
        Self.recorded.withLock { $0.append(entry) }
        let reply = Self.responder.withLock { $0 }?(entry) ?? .failure(.cannotConnectToHost)
        switch reply {
        case .hang:
            hanging.withLock { $0 = true }
        case .failure(let code):
            client?.urlProtocol(self, didFailWithError: URLError(code))
        case .response(let status, let data, let headers):
            let response = HTTPURLResponse(url: request.url!, statusCode: status, httpVersion: "HTTP/1.1", headerFields: headers)!
            client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
            client?.urlProtocol(self, didLoad: data)
            client?.urlProtocolDidFinishLoading(self)
        }
    }

    override func stopLoading() {
        if hanging.withLock({ $0 }) { Self.stoppedHanging.withLock { $0 += 1 } }
    }

    static func client() -> RelayClient {
        let configuration = RelayClient.configuration()
        configuration.protocolClasses = [StubProtocol.self]
        return RelayClient(session: URLSession(configuration: configuration))
    }
}
