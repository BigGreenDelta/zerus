package app.zerus.mobile

import java.net.URI
import java.net.URLDecoder

/** Presentation-only invitation. Neither parsing nor scanning performs network activity. */
data class PairingInvite(val server: String, val code: String) {
    override fun toString() = "PairingInvite(redacted)"
    companion object {
        const val MAX_LENGTH = 4096
        fun parse(value: String): PairingInvite? = runCatching {
            require(value.length <= MAX_LENGTH && value.none { it.isISOControl() })
            val uri = URI(value)
            require(uri.scheme == "zerus" && uri.rawAuthority == "pair" && uri.rawPath in setOf("", "/") && uri.rawFragment == null)
            val fields = linkedMapOf<String, String>()
            for (entry in requireNotNull(uri.rawQuery).split('&')) {
                val parts = entry.split('=', limit = 2)
                require(parts.size == 2)
                val key = URLDecoder.decode(parts[0], "UTF-8")
                val decoded = URLDecoder.decode(parts[1], "UTF-8")
                require(key in setOf("server", "code") && fields.put(key, decoded) == null)
            }
            val code = requireNotNull(fields["code"])
            if(WorkspaceProtocol.encryptedInvitationCode(code)) WorkspaceInvitation.parse(code)
            else require(code.length in 16..128 && code.all { it in 'A'..'Z' || it in 'a'..'z' || it in '0'..'9' || it == '-' || it == '_' })
            val server = requireNotNull(fields["server"])
            require(server.length <= 2048 && server.none { it.isWhitespace() || it.isISOControl() } && !server.contains('\uFFFD'))
            val normalized = EndpointPolicy.normalize(server)
            val endpoint = URI(normalized)
            require(endpoint.port == -1 || endpoint.port in 1..65535)
            PairingInvite(ManagedRelay.transport(normalized), code)
        }.getOrNull()
    }
}
