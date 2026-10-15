package app.zerus.mobile

import java.net.URI

/** A link belongs to the conversation computer. Parsing never reads the filesystem. */
internal data class ComputerFileReference(val path: String, val location: String = "") {
    val name: String get() = path.substringAfterLast('/').ifBlank { "File" }

    companion object {
        /** POSIX file destinations, with the same line suffixes as desktop file references. */
        fun parse(destination: String): ComputerFileReference? {
            if (destination.isBlank() || destination.any { it.isISOControl() || it == '\\' }) return null
            return runCatching {
                val uri = URI(destination.replace(" ", "%20"))
                if (uri.isOpaque || uri.rawAuthority != null || uri.rawQuery != null ||
                    uri.scheme?.lowercase()?.let { it != "file" } == true) return null
                var path = uri.path ?: return null
                var location = uri.fragment.orEmpty()
                if (path.isBlank() || path.startsWith("//") || path.toByteArray(Charsets.UTF_8).size > 8192 ||
                    path.any { it.isISOControl() || it == '\\' } || location.length > 2048 ||
                    location.any { it.isISOControl() }) return null
                if (location.isNotEmpty()) location = "#$location"
                else Regex("(:[1-9][0-9]*(?::[1-9][0-9]*)?)$").find(path)?.let { match ->
                    location = match.value
                    path = path.dropLast(location.length)
                }
                if (path.isBlank()) null else ComputerFileReference(path, location)
            }.getOrNull()
        }
    }
}
