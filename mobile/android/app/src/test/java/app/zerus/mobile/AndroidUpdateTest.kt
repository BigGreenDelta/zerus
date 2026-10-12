package app.zerus.mobile

import kotlinx.coroutines.*
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest
import java.util.concurrent.TimeUnit

class AndroidUpdateTest {
    private val cert="a".repeat(64)
    private val data="Synthetic APK bytes".repeat(100).toByteArray()
    private fun sha(bytes:ByteArray)=MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
    private fun feed()=JSONObject().put("schemaVersion",1).put("product","zerus").put("platform","android").put("channel","dev")
        .put("versionName","0.1.8").put("versionCode",9).put("releaseId",42).put("publishedAt","2026-10-09T10:00:00Z")
        .put("releaseUrl","https://github.com/ufna/zerus/releases/tag/android-dev-9").put("sourceCommit","b".repeat(40))
        .put("artifacts",JSONArray().put(JSONObject().put("kind","apk").put("name","zerus.apk")
            .put("url","https://zerus.dev/downloads/android-dev-9/zerus.apk").put("sha256",sha(data)).put("size",data.size)
            .put("os","android").put("arch","universal").put("minSdk",26).put("packageName",UpdatePolicy.PACKAGE).put("signingCertSha256",cert)))
    private fun reject(block:()->Unit) { try { block();fail("Expected rejection") } catch(_:Exception) {} }
    @Test fun validFeedRemainsForwardCompatibleWithExtraProvenance() {
        val raw=feed().put("sourceDirty",true).put("sourceSnapshot","reserved-test-snapshot")
        assertEquals(9L,UpdatePolicy.parse(raw.toString()).versionCode)
        assertEquals(cert,UpdatePolicy.parse(raw.toString()).signingCert)
    }
    @Test fun feedRejectsWrongChannelTypesAndAmbiguousArtifacts() {
        listOf("schemaVersion" to 2,"versionCode" to "9","versionCode" to 9.5,"versionCode" to -1,
            "releaseId" to 0,"product" to "other","channel" to "stable","platform" to "linux","sourceCommit" to "not-a-commit").forEach { (key,value) ->
            reject { UpdatePolicy.parse(feed().put(key,value).toString()) }
        }
        val raw=feed();raw.getJSONArray("artifacts").put(raw.getJSONArray("artifacts").getJSONObject(0))
        reject { UpdatePolicy.parse(raw.toString()) }
    }
    @Test fun trustedOriginRejectsCredentialsRedirectTargetsEncodedPathsAndTraversal() {
        listOf("http://zerus.dev/downloads/android-dev-9/zerus.apk","https://zerus.dev.evil.invalid/downloads/android-dev-9/zerus.apk",
            java.net.URI("https","user","zerus.dev",-1,"/downloads/android-dev-9/zerus.apk",null,null).toString(),"https://zerus.dev:443/downloads/android-dev-9/zerus.apk",
            "https://zerus.dev/downloads/android-dev-9/zerus.apk?token=x","https://zerus.dev/downloads/android-dev-9/%7aerus.apk",
            "https://zerus.dev/downloads/../zerus.apk").forEach { url ->
            val raw=feed();raw.getJSONArray("artifacts").getJSONObject(0).put("url",url)
            reject { UpdatePolicy.parse(raw.toString()) }
        }
        reject { UpdatePolicy.parse(feed().put("releaseUrl","https://github.com/other/zerus/releases/tag/android-dev-9").toString()) }
    }
    @Test fun feedRejectsOversizeAndWrongPackageOrFingerprint() {
        listOf("size" to UpdatePolicy.MAX_APK_BYTES+1,"minSdk" to 25,"packageName" to "other.app","sha256" to "bad","signingCertSha256" to "bad").forEach { (key,value) ->
            val raw=feed();raw.getJSONArray("artifacts").getJSONObject(0).put(key,value)
            reject { UpdatePolicy.parse(raw.toString()) }
        }
        reject { UpdatePolicy.parse(feed().put("extra","x".repeat(UpdatePolicy.MAX_FEED_BYTES)).toString()) }
    }
    @Test fun apkMustMatchAdvertisedVersionAndExactlyInstalledSigner() {
        val update=UpdatePolicy.parse(feed().toString())
        val installed=UpdateApkIdentity(UpdatePolicy.PACKAGE,8,26,setOf(cert))
        val candidate=installed.copy(versionCode=9)
        UpdatePolicy.verifyIdentity(update,candidate,installed,35)
        listOf(candidate.copy(packageName="other"),candidate.copy(versionCode=8),candidate.copy(versionCode=10),
            candidate.copy(signers=setOf("c".repeat(64))),candidate.copy(signers=setOf(cert,"c".repeat(64))),
            candidate.copy(split=true),candidate.copy(minSdk=27)).forEach { invalid -> reject { UpdatePolicy.verifyIdentity(update,invalid,installed,35) } }
        reject { UpdatePolicy.verifyIdentity(update,candidate,installed.copy(versionCode=9),35) }
        reject { UpdatePolicy.verifyIdentity(update,candidate,installed,25) }
    }
    @Test fun throttlingSurvivesRestartsAndClockRollbackButManualCheckCanBypassCaller() {
        assertTrue(UpdatePolicy.shouldCheck(0,1));assertFalse(UpdatePolicy.shouldCheck(100,101))
        assertTrue(UpdatePolicy.shouldCheck(100,100+UpdatePolicy.CHECK_INTERVAL));assertTrue(UpdatePolicy.shouldCheck(100,99))
    }
    @Test fun devRunCatalogAndStableCatalogStaySeparateInTheSamePackage() {
        val raw=feed().put("schemaVersion",2).put("buildId",12345).put("buildNumber",50).put("channelTag","dev")
            .put("buildUrl","https://github.com/ufna/zerus/actions/runs/12345")
        raw.remove("releaseId");raw.remove("releaseUrl")
        raw.getJSONArray("artifacts").getJSONObject(0).put("url","https://zerus.dev/downloads/dev-android-12345/zerus.apk")
        assertEquals(12345L,UpdatePolicy.parse(raw.toString()).releaseId)
        assertEquals("dev",UpdatePolicy.parseSaved(raw.toString()).channel)
        reject { UpdatePolicy.parse(raw.toString(),"stable") }
        for(key in listOf("buildId","buildNumber","channelTag","buildUrl")) {
            val bad=JSONObject(raw.toString()).put(key,"bad")
            reject { UpdatePolicy.parse(bad.toString()) }
        }
        reject { UpdatePolicy.parse(JSONObject(raw.toString()).put("sourceDirty",false).toString()) }
        val stable=feed().put("channel","stable")
        assertEquals("stable",UpdatePolicy.parse(stable.toString(),"stable").channel)
        assertEquals("stable",UpdatePolicy.parseSaved(stable.toString()).channel)
        reject { UpdatePolicy.parse(stable.toString(),"dev") }
        assertEquals("https://zerus.dev/updates/v2/android/dev.json",UpdatePolicy.feedUrl("dev"))
        assertEquals("https://zerus.dev/updates/v1/android/stable.json",UpdatePolicy.feedUrl("stable"))
        assertTrue(UpdatePolicy.shouldCheck(100,100+UpdatePolicy.DEV_CHECK_INTERVAL,"dev"))
        assertFalse(UpdatePolicy.shouldCheck(100,100+UpdatePolicy.DEV_CHECK_INTERVAL,"stable"))
    }
    @Test fun streamingVerifierRejectsShortOversizeAndTamperedBytes() = runBlocking {
        val out=ByteArrayOutputStream();copyVerified(ByteArrayInputStream(data),out,data.size.toLong(),sha(data));assertArrayEquals(data,out.toByteArray())
        for(bytes in listOf(data.copyOf(data.size-1),data+byteArrayOf(1),data.copyOf().apply { this[0]=1 })) {
            try { copyVerified(ByteArrayInputStream(bytes),ByteArrayOutputStream(),data.size.toLong(),sha(data));fail("Expected rejection") } catch(_:IllegalArgumentException) {}
        }
    }
    @Test fun cancelStopsUnderlyingDownloadAndRemovesPartialFilePromptly() = runBlocking {
        val server=MockWebServer();server.start()
        val partial=File.createTempFile("zerus-update-test",".part")
        try {
            server.enqueue(MockResponse().setBody(okio.Buffer().write(data)).throttleBody(1,5,TimeUnit.SECONDS))
            val client=OkHttpClient.Builder().addInterceptor { chain -> chain.proceed(chain.request().newBuilder().url(server.url("/apk")).build()) }.build()
            val downloader=UpdateDownload(client)
            val job=launch(Dispatchers.IO) { downloader.apk(UpdatePolicy.parse(feed().toString()),partial) {} }
            assertNotNull(server.takeRequest(2,TimeUnit.SECONDS))
            withTimeout(1500) { job.cancelAndJoin() }
            assertFalse(partial.exists())
        } finally { server.shutdown();partial.delete() }
    }
    @Test fun redirectsAreNotFollowedAndNoAuthenticationIsAdded() = runBlocking {
        val server=MockWebServer();server.start();val partial=File.createTempFile("zerus-update-test",".part")
        try {
            server.enqueue(MockResponse().setResponseCode(302).addHeader("Location","https://evil.invalid/update.apk"))
            val client=OkHttpClient.Builder().followRedirects(false).addInterceptor { chain -> chain.proceed(chain.request().newBuilder().url(server.url("/apk")).build()) }.build()
            try { UpdateDownload(client).apk(UpdatePolicy.parse(feed().toString()),partial) {};fail("Expected rejection") } catch(_:IllegalStateException) {}
            assertEquals(1,server.requestCount);assertNull(server.takeRequest().getHeader("Authorization"));assertFalse(partial.exists())
        } finally { server.shutdown();partial.delete() }
    }
    @Test fun pendingSessionAndForeignCallbackCannotTriggerRepeatedInstall() {
        val update=UpdatePolicy.parse(feed().toString())
        assertTrue(UpdateInstallGuard.canStart(-1,update,update));assertFalse(UpdateInstallGuard.canStart(7,update,update))
        assertFalse(UpdateInstallGuard.canStart(-1,null,update))
        assertTrue(UpdateInstallGuard.ownsCallback(7,"nonce",7,"nonce"))
        assertFalse(UpdateInstallGuard.ownsCallback(7,"nonce",8,"nonce"));assertFalse(UpdateInstallGuard.ownsCallback(7,"nonce",7,"other"))
        assertFalse(UpdateInstallGuard.ownsCallback(7,"",7,""))
    }
    @Test fun installTapOpensAndroidConfirmationOnlyForItsOwnPendingSession() {
        assertTrue(UpdateInstallGuard.opensConfirmation(7,7,"confirmation",true))
        assertFalse(UpdateInstallGuard.opensConfirmation(-1,7,"confirmation",true))
        assertFalse(UpdateInstallGuard.opensConfirmation(-1,-1,"confirmation",true))
        assertFalse(UpdateInstallGuard.opensConfirmation(6,7,"confirmation",true))
        assertFalse(UpdateInstallGuard.opensConfirmation(7,7,"installing",true))
        assertFalse(UpdateInstallGuard.opensConfirmation(7,7,"confirmation",false))
    }
    @Test fun preparationWaitsForMessagesQuestionsTerminalAndContextActionsButAllowsSavedDrafts() {
        val target=Target("computer","session","run","conversation","workspace")
        val draft=Draft(target,"Private unsent text")
        fun reason(state:MessageState)=UpdateInstallGuard.reason(state,true,false,false,false,false)
        assertEquals("",reason(MessageState(drafts=listOf(draft))))
        assertTrue(reason(MessageState(outgoing=listOf(OutgoingMessage("request",target,"text",emptyList(),1.0,"sending")))).isNotBlank())
        assertTrue(reason(MessageState(drafts=listOf(draft.copy(questionId="question",status="submitting")))).isNotBlank())
        listOf("terminal_input","settings","launch","pause").forEach { operation ->
            assertTrue(reason(MessageState(sessionActions=listOf(SessionAction("request",target,operation,"{}")))).isNotBlank())
        }
        assertTrue(UpdateInstallGuard.reason(MessageState(),true,false,true,false,false).isNotBlank())
        assertTrue(UpdateInstallGuard.reason(MessageState(),true,false,false,true,false).isNotBlank())
        assertTrue(UpdateInstallGuard.reason(MessageState(),true,false,false,false,true).isNotBlank())
        assertTrue(UpdateInstallGuard.reason(MessageState(),false,false,false,false,false).isNotBlank())
        assertTrue(UpdateInstallGuard.reason(MessageState(),true,true,false,false,false).isNotBlank())
    }
}
