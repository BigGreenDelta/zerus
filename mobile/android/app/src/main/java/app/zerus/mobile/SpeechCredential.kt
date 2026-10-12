package app.zerus.mobile

import org.json.JSONObject
import java.security.AlgorithmParameters
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.X509EncodedKeySpec
import java.util.Base64
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/** Standard P256 ECDH / HKDF-SHA256 / AES-GCM envelope bound to the exact edit. */
internal object SpeechCredential {
    const val algorithm = "P256-HKDF-SHA256-A256GCM"
    fun context(payload: JSONObject): ByteArray {
        val profile = payload.getJSONObject("profile")
        return listOf("zerus-speech-credential-v1", payload.getString("expected_machine_id"), payload.getString("request_id"),
            "profile_upsert", payload.getLong("expected_revision").toString(), profile.getString("id"), profile.getString("name"),
            profile.getString("adapter"), profile.getString("model"), profile.getString("language"), profile.string("device"),
            profile.string("base_url"), profile.optBoolean("allow_insecure_http").toString()).joinToString("\n") {
                require(it.none { char -> char.code < 32 || char.code == 127 }); it
            }.toByteArray(Charsets.UTF_8)
    }
    private fun hmac(key: ByteArray, value: ByteArray) = Mac.getInstance("HmacSHA256").run {
        init(SecretKeySpec(key, "HmacSHA256")); doFinal(value)
    }
    fun seal(key: SpeechConfigKey, payload: JSONObject, secret: String): JSONObject {
        val plaintext = secret.toByteArray(Charsets.UTF_8)
        try {
            require(plaintext.size in 1..4096 && secret.none { it.code < 32 || it.code == 127 }) { "Enter a valid API key (up to 4096 bytes)." }
            require(key.algorithm == algorithm) { "Update Zerus to use this computer's credential encryption." }
            val der = Base64.getDecoder().decode(key.publicSpki); require(der.size in 1..128 && Base64.getEncoder().encodeToString(der) == key.publicSpki)
            val hash = MessageDigest.getInstance("SHA-256").digest(der)
            require(hash.joinToString("") { "%02x".format(it) } == key.id) { "The computer returned an invalid credential key." }
            val public = KeyFactory.getInstance("EC").generatePublic(X509EncodedKeySpec(der)) as ECPublicKey
            val p256 = AlgorithmParameters.getInstance("EC").apply { init(ECGenParameterSpec("secp256r1")) }.getParameterSpec(ECParameterSpec::class.java)
            require(public.encoded.contentEquals(der) && public.params.curve == p256.curve && public.params.generator == p256.generator &&
                public.params.order == p256.order && public.params.cofactor == p256.cofactor)
            val ephemeral = KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1")) }.generateKeyPair()
            val shared = KeyAgreement.getInstance("ECDH").run { init(ephemeral.private); doPhase(public, true); generateSecret() }
            require(shared.size == 32)
            val context = context(payload)
            val prk = hmac(hash, shared); shared.fill(0)
            val aesKey = hmac(prk, context + byteArrayOf(1)); prk.fill(0)
            val nonce = ByteArray(12).also { SecureRandom().nextBytes(it) }
            val encrypted = try {
                Cipher.getInstance("AES/GCM/NoPadding").run {
                    init(Cipher.ENCRYPT_MODE, SecretKeySpec(aesKey, "AES"), GCMParameterSpec(128, nonce))
                    updateAAD(context); doFinal(plaintext)
                }
            } finally { aesKey.fill(0) }
            fun encoded(value: ByteArray) = Base64.getEncoder().encodeToString(value)
            return JSONObject().put("key_id", key.id).put("algorithm", algorithm)
                .put("ephemeral_public_key", encoded(ephemeral.public.encoded)).put("nonce", encoded(nonce)).put("ciphertext", encoded(encrypted))
        } finally { plaintext.fill(0) }
    }
}
