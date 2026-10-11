package app.zerus.mobile

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.cancelAndJoin
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import java.io.File
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.security.MessageDigest
import java.util.concurrent.TimeUnit

/** No relay interceptors, credentials, redirects or client identifiers. */
class UpdateDownload(private val client: OkHttpClient = OkHttpClient.Builder().followRedirects(false)
    .followSslRedirects(false).connectTimeout(15,TimeUnit.SECONDS).readTimeout(15,TimeUnit.SECONDS)
    .callTimeout(10,TimeUnit.MINUTES).build()) {
    private suspend fun <T> request(client: OkHttpClient, request: Request, read: suspend (Response) -> T): T = coroutineScope {
        val call=client.newCall(request)
        val cancellation=launch(start=CoroutineStart.UNDISPATCHED) { try { awaitCancellation() } finally { call.cancel() } }
        try { call.execute().use { read(it) } }
        finally { withContext(NonCancellable) { cancellation.cancelAndJoin() } }
    }
    suspend fun feed(channel: String = "dev"): String = withContext(Dispatchers.IO) {
        request(client.newBuilder().callTimeout(30,TimeUnit.SECONDS).build(),Request.Builder().url(UpdatePolicy.feedUrl(channel)).header("Accept","application/json").build()) { response ->
            check(response.code == 200) { "Update check is unavailable. Try again later." }
            val body = checkNotNull(response.body)
            require(body.contentLength() <= UpdatePolicy.MAX_FEED_BYTES)
            body.byteStream().use { input ->
                val bytes=ByteArrayOutputStream();val buffer=ByteArray(8192)
                while(true) {
                    currentCoroutineContext().ensureActive()
                    val count=input.read(buffer);if(count < 0) break
                    require(bytes.size()+count <= UpdatePolicy.MAX_FEED_BYTES) { "Update metadata is too large." }
                    bytes.write(buffer,0,count)
                }
                bytes.toString("UTF-8")
            }
        }
    }
    suspend fun apk(update: AndroidUpdate, partial: File, onProgress: (Long) -> Unit) = withContext(Dispatchers.IO) {
        try {
            request(client,Request.Builder().url(update.url).header("Accept-Encoding","identity").build()) { response ->
                check(response.code == 200) { "Update download is unavailable. Try again later." }
                val body = checkNotNull(response.body)
                require(body.contentLength() == -1L || body.contentLength() == update.size) { "Unexpected update file size." }
                body.byteStream().use { input -> partial.outputStream().use { output ->
                    copyVerified(input,output,update.size,update.sha256) { count -> currentCoroutineContext().ensureActive();onProgress(count) }
                    output.fd.sync()
                } }
            }
        } catch (e: Throwable) { partial.delete();currentCoroutineContext().ensureActive();throw e }
    }
}

/** Bounded byte verifier shared by download and pre-install revalidation. */
suspend fun copyVerified(input: InputStream, output: OutputStream, expectedSize: Long, expectedSha: String,
    onProgress: suspend (Long) -> Unit = {}) {
    require(expectedSize in 1..UpdatePolicy.MAX_APK_BYTES)
    val digest = MessageDigest.getInstance("SHA-256")
    val buffer = ByteArray(64 * 1024);var total = 0L
    while(true) {
        currentCoroutineContext().ensureActive()
        val count = input.read(buffer); if(count < 0) break
        if(count == 0) continue
        total += count;require(total <= expectedSize) { "Update file exceeds its advertised size." }
        digest.update(buffer,0,count);output.write(buffer,0,count);onProgress(total)
    }
    require(total == expectedSize) { "Update download is incomplete." }
    val sha = digest.digest().joinToString("") { "%02x".format(it) }
    require(sha == expectedSha) { "Update file fingerprint does not match." }
}
