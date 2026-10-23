import Foundation

/// The connection remains retryable until protected cleanup and its final index save succeed.
public enum ConnectionRemoval {
    public static func forget(_ connection: Connection, from current: [Connection],
                              eraseProtected: @Sendable (Connection) async throws -> Void,
                              saveIndex: @Sendable ([Connection]) async throws -> Void) async throws -> [Connection] {
        guard current.contains(connection) else {
            throw ZerusError.mismatch("The connection changed before it could be forgotten. Refresh and try again.")
        }
        if connection.protocolVersion == 2 { try await eraseProtected(connection) }
        let next = current.filter { $0.id != connection.id }
        // An index failure preserves the old actionable row; protected cleanup is idempotent.
        try await saveIndex(next)
        return next
    }
}
