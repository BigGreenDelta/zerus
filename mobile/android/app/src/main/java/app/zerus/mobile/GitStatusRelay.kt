package app.zerus.mobile

import org.json.JSONObject

/** Wire transport only. The caller owns access/connection-epoch admission and receipt polling. */
internal class GitStatusRelay(private val api: RelayApi) {
    suspend fun start(connection: Connection, request: GitStatusRequest): JSONObject {
        validate(connection, request)
        return checked(api.post(connection,request.key.machine.computerId,"",GitStatus.OPERATION,request.payload(),request.requestId),request)
    }

    suspend fun receipt(connection: Connection, request: GitStatusRequest): JSONObject {
        validate(connection, request)
        return checked(api.receipt(connection, request.requestId), request)
    }

    private fun validate(connection: Connection, request: GitStatusRequest) {
        require(connection.id == request.key.machine.connectionId) { "Git status connection does not match the original request." }
        require(GitStatus.validKey(request.key) && GitStatus.canonicalId(request.requestId)) { "Invalid Git status request." }
    }

    private fun checked(envelope: JSONObject, request: GitStatusRequest): JSONObject = envelope.also {
        check(it.opt("request_id") as? String == request.requestId) {
            "Gateway returned a mismatched receipt. Delivery remains unconfirmed."
        }
    }
}
