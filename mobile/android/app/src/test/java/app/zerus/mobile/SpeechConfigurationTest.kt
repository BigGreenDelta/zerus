package app.zerus.mobile

import kotlinx.coroutines.runBlocking
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.spec.ECGenParameterSpec
import java.security.spec.X509EncodedKeySpec
import java.security.spec.PKCS8EncodedKeySpec
import java.io.File
import java.util.Base64
import java.util.UUID
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

class SpeechConfigurationTest {
    private val host = SpeechConfigHost(MachineKey("workspace", "relay-node"), "b4075bf0-9c22-4a67-b768-55af2d2936a4", "GPU")
    private fun keyPair() = KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1")) }.generateKeyPair()
    private fun metadata(pair: KeyPair) = SpeechConfigKey(MessageDigest.getInstance("SHA-256").digest(pair.public.encoded).joinToString("") { "%02x".format(it) },
        SpeechCredential.algorithm, Base64.getEncoder().encodeToString(pair.public.encoded))
    private fun config(key: SpeechConfigKey? = null) = SpeechConfiguration(host.machineId, 3, true, "",
        listOf(SpeechConfigAdapter("gigaam", listOf("v3_e2e_rnnt"), listOf("cpu", "cuda")), SpeechConfigAdapter("openai_http", emptyList(), emptyList())), emptyList(), key)
    private fun profile() = SpeechConfigProfile("remote", 0, "Remote Russian", "openai_http", "gigaam", "ru", baseUrl = "https://speech.example.test/v1")
    private fun decrypt(pair: KeyPair, payload: JSONObject, sealed: JSONObject): String {
        val decoder = Base64.getDecoder()
        val ephemeral = KeyFactory.getInstance("EC").generatePublic(X509EncodedKeySpec(decoder.decode(sealed.getString("ephemeral_public_key"))))
        val shared = KeyAgreement.getInstance("ECDH").run { init(pair.private); doPhase(ephemeral, true); generateSecret() }
        val salt = MessageDigest.getInstance("SHA-256").digest(pair.public.encoded)
        fun hmac(key: ByteArray, input: ByteArray) = Mac.getInstance("HmacSHA256").run { init(SecretKeySpec(key, "HmacSHA256")); doFinal(input) }
        val context = SpeechCredential.context(payload)
        val key = hmac(hmac(salt, shared), context + byteArrayOf(1))
        return Cipher.getInstance("AES/GCM/NoPadding").run {
            init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, decoder.decode(sealed.getString("nonce"))))
            updateAAD(context); String(doFinal(decoder.decode(sealed.getString("ciphertext"))), Charsets.UTF_8)
        }
    }
    @Test fun sharedPythonAndRustKnownAnswerDecryptsWithJavaCrypto() {
        val fixture = JSONObject(File("../../../tests/fixtures/speech-credential-v1.json").readText())
        val factory = KeyFactory.getInstance("EC"); val decoder = Base64.getDecoder()
        val pair = KeyPair(factory.generatePublic(X509EncodedKeySpec(decoder.decode(fixture.getString("host_public_spki")))),
            factory.generatePrivate(PKCS8EncodedKeySpec(decoder.decode(fixture.getString("host_private_pkcs8")))))
        val request = fixture.getJSONObject("request")
        assertArrayEquals(decoder.decode(fixture.getString("context_base64")), SpeechCredential.context(request))
        assertEquals(fixture.getString("plaintext"), decrypt(pair, request, request.getJSONObject("credential").getJSONObject("sealed_value")))
        val produced = SpeechCredential.seal(metadata(pair), request, fixture.getString("plaintext"))
        assertEquals(fixture.getString("plaintext"), decrypt(pair, request, produced))
    }
    @Test fun fieldsUseUtf8ByteBoundsOptionalLanguageAndExactIntegerRevisions() {
        SpeechConfigProtocol.validate(profile().copy(language = ""), config())
        SpeechConfigProtocol.validate(profile().copy(language = "en"), config())
        for (language in listOf("RU", "rus", "r", " ru", "ру"))
            assertThrows(IllegalArgumentException::class.java) { SpeechConfigProtocol.validate(profile().copy(language = language), config()) }
        assertThrows(IllegalArgumentException::class.java) { SpeechConfigProtocol.validate(profile().copy(name = "я".repeat(65)), config()) }
        assertThrows(IllegalArgumentException::class.java) { SpeechConfigProtocol.validate(profile().copy(language = "я".repeat(17)), config()) }
        for (value in listOf<Any>(1.5, "7", true, -1L, 9_007_199_254_740_992L))
            assertThrows(IllegalArgumentException::class.java) { SpeechConfigProtocol.revision(JSONObject().put("revision", value), "revision") }
        assertEquals(0L, SpeechConfigProtocol.revision(JSONObject().put("revision", 0), "revision"))
    }
    @Test fun sealedKeyBindsEveryProfileFieldHostRevisionAndOperation() {
        val pair = keyPair(); val secret = "synthetic-test-key"
        val op = SpeechConfigProtocol.upsert(host, config(metadata(pair)), profile(), "replace", secret)
        assertFalse(op.payload.contains(secret))
        val payload = JSONObject(op.payload); val sealed = payload.getJSONObject("credential").getJSONObject("sealed_value")
        assertEquals(secret, decrypt(pair, payload, sealed))
        for (field in listOf("expected_machine_id", "request_id", "expected_revision")) {
            val changed = JSONObject(op.payload).put(field, if (field == "expected_revision") 4 else UUID.randomUUID().toString())
            assertThrows(AEADBadTagException::class.java) { decrypt(pair, changed, sealed) }
        }
        for (field in listOf("id", "name", "adapter", "model", "language", "device", "base_url", "allow_insecure_http")) {
            val changed = JSONObject(op.payload)
            changed.getJSONObject("profile").put(field, if (field == "allow_insecure_http") true else "changed")
            assertThrows(AEADBadTagException::class.java) { decrypt(pair, changed, sealed) }
        }
        assertThrows(AEADBadTagException::class.java) { decrypt(keyPair(), payload, sealed) }
        val recovered = SpeechConfigProtocol.decode(SpeechConfigProtocol.encode(listOf(op))).single()
        assertEquals(op, recovered)
        assertFalse(SpeechConfigProtocol.encode(listOf(op)).toString().contains(secret))
        assertEquals(secret, decrypt(pair, JSONObject(recovered.payload), sealed))
    }
    @Test fun rejectsMismatchedKeyUnsupportedCurveControlFieldsAndUrlCredentials() {
        val pair = keyPair(); val key = metadata(pair)
        assertThrows(IllegalArgumentException::class.java) { SpeechConfigProtocol.upsert(host, config(key.copy(id = "0".repeat(64))), profile(), "replace", "synthetic") }
        assertThrows(IllegalArgumentException::class.java) { SpeechConfigProtocol.upsert(host, config(key), profile().copy(name = "a\nb"), "replace", "synthetic") }
        assertThrows(IllegalArgumentException::class.java) { SpeechConfigProtocol.upsert(host, config(key), profile(), "replace", "synthetic\nkey") }
        for (url in listOf("https://user:password@speech.example.test/v1", "https://speech.example.test/v1?key=test", "https://speech.example.test/v1#key", "http://127.0.0.1/v1")) {
            assertThrows(IllegalArgumentException::class.java) { SpeechConfigProtocol.validate(profile().copy(baseUrl = url), config()) }
        }
        SpeechConfigProtocol.validate(profile().copy(baseUrl = "http://127.0.0.1:8000/v1", allowInsecureHttp = true), config())
        val local = SpeechConfigProfile("local", 1, "Local", "gigaam", "v3_e2e_rnnt", "ru", device = "cuda")
        SpeechConfigProtocol.validate(local, config())
        assertThrows(IllegalArgumentException::class.java) { SpeechConfigProtocol.validate(local.copy(device = "/tmp/executable"), config()) }
        val otherCurve = KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp384r1")) }.generateKeyPair()
        assertThrows(IllegalArgumentException::class.java) { SpeechConfigProtocol.upsert(host, config(metadata(otherCurve)), profile(), "replace", "synthetic") }
    }
    @Test fun configurationMutationStatusAndCasStayBoundToTheirExactHost() {
        val op = SpeechConfigProtocol.remove(host, config(), profile().copy(revision = 2))
        val payload = JSONObject(op.payload)
        assertEquals(3, payload.getInt("expected_revision")); assertEquals(host.machineId, payload.getString("expected_machine_id"))
        val test = SpeechConfigProtocol.test(host, profile().copy(revision = 2))
        assertEquals(2, JSONObject(test.payload).getInt("expected_profile_revision"))
        val result = JSONObject().put("schema", 1).put("machine_id", host.machineId).put("request_id", op.id).put("state", "failed")
            .put("error_code", "revision_conflict")
        assertEquals("revision_conflict", SpeechConfigProtocol.result(result, op).errorCode)
        assertThrows(IllegalArgumentException::class.java) { SpeechConfigProtocol.result(JSONObject(result.toString()).put("request_id", UUID.randomUUID().toString()), op) }
        assertThrows(IllegalArgumentException::class.java) { SpeechConfigProtocol.result(JSONObject(result.toString()).put("machine_id", UUID.randomUUID().toString()), op) }
        val list = JSONObject().put("schema", 1).put("machine_id", host.machineId).put("revision", 0).put("service", JSONObject().put("available", false))
            .put("adapters", JSONArray()).put("profiles", JSONArray())
        assertFalse(SpeechConfigProtocol.parse(list, host).serviceAvailable)
        assertThrows(IllegalArgumentException::class.java) { SpeechConfigProtocol.parse(list, host.copy(machineId = UUID.randomUUID().toString())) }
    }
    @Test fun configurationEnvelopeHasNoSessionOrConversationAndNoPlaintextCredential() = runBlocking {
        MockWebServer().use { server ->
            server.start()
            val pair = keyPair(); val op = SpeechConfigProtocol.upsert(host, config(metadata(pair)), profile(), "replace", "synthetic-secret")
            val response = JSONObject().put("schema", 1).put("machine_id", host.machineId).put("request_id", op.id).put("state", "completed")
            server.enqueue(MockResponse().setBody(JSONObject().put("request_id", op.id).put("state", "completed").put("result", response).toString()))
            val connection = Connection(host.key.connectionId, "Fixture", server.url("/").toString().removeSuffix("/"), "fixture-token")
            RelayApi(OkHttpClient()).speechConfiguration(connection, host, op.operation, JSONObject(op.payload), op.id)
            val request = server.takeRequest().body.readUtf8(); val envelope = JSONObject(request)
            assertEquals("", envelope.getString("session")); assertEquals(host.key.computerId, envelope.getString("computer_id"))
            assertEquals(op.id, envelope.getJSONObject("payload").getString("request_id")); assertFalse(request.contains("synthetic-secret"))
            assertEquals("synthetic-secret", decrypt(pair, envelope.getJSONObject("payload"), envelope.getJSONObject("payload").getJSONObject("credential").getJSONObject("sealed_value")))
        }
    }
}
