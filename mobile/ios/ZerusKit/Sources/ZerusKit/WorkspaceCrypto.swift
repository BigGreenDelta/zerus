import CryptoKit
import Foundation

/// RFC 9180 single-shot Base mode; no persistent HPKE sequence or nonce state.
/// WorkspaceProtocol owns framing and membership. Header bytes are HPKE info and
/// AEAD additional data is empty, matching Tink's interoperable RAW format.
public enum WorkspaceCrypto {
    public static let suite = HPKE.Ciphersuite(kem: .Curve25519_HKDF_SHA256, kdf: .HKDF_SHA256, aead: .AES_GCM_256)

    /// Private material has no textual description and belongs only in ThisDeviceOnly storage.
    public struct Keys: Sendable {
        let signing: Curve25519.Signing.PrivateKey
        let encryption: Curve25519.KeyAgreement.PrivateKey

        public init() {
            signing = Curve25519.Signing.PrivateKey()
            encryption = Curve25519.KeyAgreement.PrivateKey()
        }

        public init(signingSeed: Data, encryptionPrivate: Data) throws {
            signing = try Curve25519.Signing.PrivateKey(rawRepresentation: signingSeed)
            encryption = try Curve25519.KeyAgreement.PrivateKey(rawRepresentation: encryptionPrivate)
        }

        public var signingPublic: Data { signing.publicKey.rawRepresentation }
        public var encryptionPublic: Data { encryption.publicKey.rawRepresentation }
        var signingSeed: Data { signing.rawRepresentation }
        var encryptionPrivate: Data { encryption.rawRepresentation }
    }

    public static func sign(_ keys: Keys, message: Data) throws -> Data {
        try keys.signing.signature(for: message)
    }

    public static func verify(publicKey: Data, signature: Data, message: Data) throws {
        guard signature.count == 64, !WorkspaceRejectedKeys.signing.contains(WorkspaceProtocol.encode(publicKey)),
              try Curve25519.Signing.PublicKey(rawRepresentation: publicKey).isValidSignature(signature, for: message) else {
            throw ZerusError.mismatch("Invalid workspace signature.")
        }
    }

    /// The first 32 bytes are RFC9180 enc; the remainder is ciphertext and tag.
    public static func seal(recipientPublic: Data, info: Data, plaintext: Data) throws -> Data {
        let key = try Curve25519.KeyAgreement.PublicKey(rawRepresentation: recipientPublic)
        var sender = try HPKE.Sender(recipientKey: key, ciphersuite: suite, info: info)
        return try sender.encapsulatedKey + sender.seal(plaintext)
    }

    public static func open(_ keys: Keys, info: Data, sealed: Data) throws -> Data {
        guard sealed.count >= 48 else { throw ZerusError.invalid("Invalid encrypted workspace payload.") }
        var recipient = try HPKE.Recipient(privateKey: keys.encryption, ciphersuite: suite, info: info,
                                           encapsulatedKey: Data(sealed.prefix(32)))
        return try recipient.open(Data(sealed.dropFirst(32)))
    }

    public static func hmac(secret: Data, message: Data) throws -> Data {
        guard secret.count == 32 else { throw ZerusError.invalid("Invalid workspace invitation.") }
        return Data(HMAC<SHA256>.authenticationCode(for: message, using: SymmetricKey(data: secret)))
    }
}
