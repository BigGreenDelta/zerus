package app.zerus.mobile

import com.google.crypto.tink.HybridDecrypt
import com.google.crypto.tink.HybridEncrypt
import com.google.crypto.tink.InsecureSecretKeyAccess
import com.google.crypto.tink.KeysetHandle
import com.google.crypto.tink.hybrid.HpkeParameters
import com.google.crypto.tink.hybrid.HpkePrivateKey
import com.google.crypto.tink.hybrid.HpkePublicKey
import com.google.crypto.tink.hybrid.HybridConfig
import com.google.crypto.tink.subtle.Ed25519Sign
import com.google.crypto.tink.subtle.Ed25519Verify
import com.google.crypto.tink.util.Bytes
import com.google.crypto.tink.util.SecretBytes
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/** RFC 9180 single-shot Base mode. Framing and membership live in WorkspaceProtocol.
 * Each call creates a fresh HPKE context; no nonce or sequence can be reused after a crash.
 * Keys are serialized only into the existing Keystore-encrypted private store.
 */
internal object WorkspaceCrypto {
    private val access = InsecureSecretKeyAccess.get()
    private val parameters = HpkeParameters.builder()
        .setKemId(HpkeParameters.KemId.DHKEM_X25519_HKDF_SHA256)
        .setKdfId(HpkeParameters.KdfId.HKDF_SHA256)
        .setAeadId(HpkeParameters.AeadId.AES_256_GCM)
        .setVariant(HpkeParameters.Variant.NO_PREFIX).build()

    init { HybridConfig.register() }

    /** Never use a data class: private material must not appear in generated toString(). */
    class Keys internal constructor(
        internal val signingSeed: ByteArray,
        internal val encryptionPrivate: ByteArray,
        val signingPublic: ByteArray,
        val encryptionPublic: ByteArray,
    )

    fun generate(): Keys {
        val signing = Ed25519Sign.KeyPair.newKeyPair()
        val encryption = KeysetHandle.generateNew(parameters).getAt(0).key as HpkePrivateKey
        return Keys(signing.privateKey, encryption.privateKeyBytes.toByteArray(access), signing.publicKey,
            encryption.publicKey.publicKeyBytes.toByteArray())
    }

    fun restore(signingSeed: ByteArray, encryptionPrivate: ByteArray, encryptionPublic: ByteArray): Keys {
        require(signingSeed.size == 32 && encryptionPrivate.size == 32 && encryptionPublic.size == 32)
        // Tink validates that the public key matches its private key.
        privateKey(encryptionPrivate, encryptionPublic)
        val signing = Ed25519Sign.KeyPair.newKeyPairFromSeed(signingSeed)
        return Keys(signingSeed.copyOf(), encryptionPrivate.copyOf(), signing.publicKey, encryptionPublic.copyOf())
    }

    fun sign(keys: Keys, message: ByteArray): ByteArray = Ed25519Sign(keys.signingSeed).sign(message)

    fun verify(publicKey: ByteArray, signature: ByteArray, message: ByteArray) {
        require(publicKey.size == 32 && signature.size == 64) { "Invalid workspace signature." }
        require(WorkspaceProtocol.encode(publicKey) !in WorkspaceRejectedKeys.signing) { "Invalid workspace signing key." }
        Ed25519Verify(publicKey).verify(signature, message)
    }

    /** RAW Tink wire format is the RFC enc(32) followed by ciphertext and tag. Header is info; AAD is empty. */
    fun seal(recipientPublic: ByteArray, info: ByteArray, plaintext: ByteArray): ByteArray {
        require(recipientPublic.size == 32)
        val key = HpkePublicKey.create(parameters, Bytes.copyFrom(recipientPublic), null)
        return handle(key).getPrimitive(HybridEncrypt::class.java).encrypt(plaintext, info)
    }

    fun open(keys: Keys, info: ByteArray, sealed: ByteArray): ByteArray {
        require(sealed.size >= 48) { "Invalid encrypted workspace payload." }
        return handle(privateKey(keys.encryptionPrivate, keys.encryptionPublic))
            .getPrimitive(HybridDecrypt::class.java).decrypt(sealed, info)
    }

    fun hmac(secret: ByteArray, message: ByteArray): ByteArray {
        require(secret.size == 32)
        return Mac.getInstance("HmacSHA256").apply { init(SecretKeySpec(secret, "HmacSHA256")) }.doFinal(message)
    }

    private fun privateKey(privateBytes: ByteArray, publicBytes: ByteArray): HpkePrivateKey = HpkePrivateKey.create(
        HpkePublicKey.create(parameters, Bytes.copyFrom(publicBytes), null), SecretBytes.copyFrom(privateBytes, access))

    private fun handle(key: com.google.crypto.tink.Key) = KeysetHandle.newBuilder().addEntry(
        KeysetHandle.importKey(key).makePrimary().withFixedId(1)).build()
}
