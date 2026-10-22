package app.zerus.mobile

import java.net.URI

/**
 * What a confirmed Terminal link may do. Web links open in the browser and file links read
 * from the session's computer through file_read; every other target can only be copied.
 */
internal sealed interface TerminalLinkTarget {
    val uri: String
    data class Web(override val uri: String) : TerminalLinkTarget
    /** [host] is the link's own host name; the path is read on the session's computer. */
    data class File(override val uri: String, val host: String, val reference: ComputerFileReference) : TerminalLinkTarget
    data class Other(override val uri: String) : TerminalLinkTarget

    companion object {
        fun of(uri: String): TerminalLinkTarget {
            safeMarkdownUrl(uri)?.let { return Web(it) }
            runCatching {
                val parsed = URI(uri)
                if (parsed.scheme?.lowercase() == "file" && !parsed.isOpaque && parsed.rawQuery == null &&
                    parsed.rawUserInfo == null && parsed.port == -1) {
                    val destination = "file:" + parsed.rawPath.orEmpty() + (parsed.rawFragment?.let { "#$it" } ?: "")
                    ComputerFileReference.parse(destination)?.let { return File(uri, parsed.host ?: parsed.rawAuthority.orEmpty(), it) }
                }
            }
            return Other(uri)
        }
    }
}

/** A tapped link with the text it was shown as. */
internal data class TerminalLinkChoice(val target: TerminalLinkTarget, val label: String)
