package app.zerus.mobile

import org.json.JSONObject
import java.net.URI
import java.time.Instant

data class AndroidUpdate(val raw: String, val versionName: String, val versionCode: Long, val releaseId: Long,
    val releaseUrl: String, val name: String, val url: String, val sha256: String, val size: Long,
    val minSdk: Int, val signingCert: String, val channel: String = "dev")
data class UpdateApkIdentity(val packageName: String, val versionCode: Long, val minSdk: Int,
    val signers: Set<String>, val split: Boolean = false)

object UpdatePolicy {
    const val FEED_URL = "https://zerus.dev/updates/v1/android/dev.json"
    const val PACKAGE = "app.zerus.mobile"
    const val MAX_FEED_BYTES = 64 * 1024
    const val MAX_APK_BYTES = 128L * 1024 * 1024
    const val CHECK_INTERVAL = 6L * 60 * 60 * 1000
    const val DEV_CHECK_INTERVAL = 15L * 60 * 1000
    fun feedUrl(channel: String): String {
        require(channel in setOf("stable", "dev"))
        return "https://zerus.dev/updates/" + (if(channel == "dev") "v2" else "v1") + "/android/$channel.json"
    }
    private val hex = Regex("[0-9a-fA-F]{64}")
    private val component = Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,159}")
    private fun string(raw: JSONObject, key: String) = (raw.opt(key) as? String) ?: error("Invalid update metadata.")
    private fun integer(raw: JSONObject, key: String): Long = try {
        (raw.opt(key) as? Number)?.toString()?.toBigDecimal()?.longValueExact() ?: error("Invalid update metadata.")
    } catch (_: ArithmeticException) { error("Invalid update metadata.") }
    private fun trusted(url: String, host: String): URI {
        val uri = URI(url)
        require(uri.scheme == "https" && uri.host == host && uri.port == -1 && uri.rawUserInfo == null &&
            uri.rawQuery == null && uri.rawFragment == null) { "Untrusted update address." }
        return uri
    }
    fun parse(text: String): AndroidUpdate = parse(text, "dev")
    fun parseSaved(text: String): AndroidUpdate = parse(text, JSONObject(text).getString("channel"))
    fun parse(text: String, channel: String): AndroidUpdate {
        require(text.toByteArray(Charsets.UTF_8).size <= MAX_FEED_BYTES) { "Update metadata is too large." }
        val raw = JSONObject(text)
        val schema = integer(raw,"schemaVersion")
        require(channel in setOf("stable", "dev") && (schema == 1L || schema == 2L && channel == "dev") &&
            string(raw,"product") == "zerus" && string(raw,"platform") == "android" &&
            string(raw,"channel") == channel) { "Unsupported update channel." }
        val version = string(raw,"versionName"); require(version.isNotBlank() && version.length <= 80 && version.none(Char::isISOControl))
        val code = integer(raw,"versionCode"); require(code in 1..Int.MAX_VALUE.toLong())
        val releaseId = integer(raw,if(schema == 2L) "buildId" else "releaseId"); require(releaseId in 1..9007199254740991L)
        Instant.parse(string(raw,"publishedAt"))
        require(Regex("[0-9a-fA-F]{40}").matches(string(raw,"sourceCommit")))
        val release = string(raw,if(schema == 2L) "buildUrl" else "releaseUrl")
        val path = trusted(release,"github.com").rawPath
        val tag = if(schema == 2L) {
            require(path == "/ufna/zerus/actions/runs/$releaseId" && string(raw,"channelTag") == "dev" &&
                integer(raw,"buildNumber") > 0 && !raw.has("sourceDirty")) { "Untrusted dev build identity." }
            "dev-android-$releaseId"
        } else {
            val prefix = "/ufna/zerus/releases/tag/"
            require(path.startsWith(prefix)) { "Untrusted release address." }
            path.removePrefix(prefix).also { require(component.matches(it) && !it.contains("..")) }
        }
        val artifacts = raw.getJSONArray("artifacts"); require(artifacts.length() in 1..16)
        val apk = (0 until artifacts.length()).map { artifacts.getJSONObject(it) }.filter {
            it.opt("kind") == "apk" && it.opt("os") == "android" && it.opt("arch") == "universal"
        }.single()
        require(string(apk,"packageName") == PACKAGE) { "Update belongs to another app." }
        val name = string(apk,"name"); require(component.matches(name) && name.endsWith(".apk") && !name.contains(".."))
        val url = string(apk,"url")
        require(trusted(url,"zerus.dev").rawPath == "/downloads/$tag/$name") { "Untrusted download address." }
        val sha = string(apk,"sha256"); val cert = string(apk,"signingCertSha256")
        require(hex.matches(sha) && hex.matches(cert)) { "Invalid update fingerprint." }
        val size = integer(apk,"size"); require(size in 1..MAX_APK_BYTES) { "Update file is too large." }
        val sdk = integer(apk,"minSdk"); require(sdk in 26..Int.MAX_VALUE.toLong())
        return AndroidUpdate(text,version,code,releaseId,release,name,url,sha.lowercase(),size,sdk.toInt(),cert.lowercase(),channel)
    }
    fun verifyIdentity(update: AndroidUpdate, candidate: UpdateApkIdentity, installed: UpdateApkIdentity, sdk: Int) {
        require(installed.packageName == PACKAGE && candidate.packageName == PACKAGE && !candidate.split) { "Unexpected APK package." }
        require(update.versionCode > installed.versionCode && candidate.versionCode == update.versionCode) { "The APK version is not a newer advertised update." }
        require(candidate.minSdk == update.minSdk && sdk >= update.minSdk) { "This update requires a newer Android version." }
        require(installed.signers.isNotEmpty() && candidate.signers == installed.signers &&
            candidate.signers == setOf(update.signingCert)) { "The APK signing key does not match this installation." }
    }
    fun shouldCheck(lastAttempt: Long, now: Long, channel: String = "stable") = lastAttempt == 0L || now < lastAttempt ||
        now - lastAttempt >= (if(channel == "dev") DEV_CHECK_INTERVAL else CHECK_INTERVAL)
}
