# Encrypted relay endpoint primitives

This library defines the first encrypted payload format for relay API v2.
It does not enable encryption in API v1. Protected connections must fail closed
when keys or membership are unavailable; they must never retry using plaintext.
The HTTP relay routes ciphertext and manages independent bearer credentials.
Endpoints own device authorization and private keys.

## Cryptography selection

Each payload uses a fresh, single-shot [RFC 9180 HPKE](https://www.rfc-editor.org/rfc/rfc9180.html)
Base context with DHKEM(X25519, HKDF-SHA256), HKDF-SHA256 and AES-256-GCM
(KEM `0x0020`, KDF `0x0001`, AEAD `0x0002`). An independent
[RFC 8032 Ed25519](https://www.rfc-editor.org/rfc/rfc8032.html) signature
authenticates the complete routing header, encapsulated key and ciphertext.
The recipient key and sender signing key come from endpoint-verified membership,
never a relay-controlled key directory. Signing and encryption keys are separate.
Public encodings are canonical unpadded base64url. Ed25519 encodings with
noncanonical `y` or a small-order point are rejected; signatures use strict
verification. Integer fields are JSON integers in `0..2^63-1`; key generations
and expiry are positive.

Rust uses `hpke` and `ed25519-dalek`, native Android uses Google Tink, and iOS
uses CryptoKit. HPKE `info` contains the complete canonical header and AEAD AAD
is empty. This matches Tink's hybrid-encryption context-info interface and its
RAW `enc32 || ciphertext || tag16` representation. This library keeps `enc`
and ciphertext in separate JSON fields. The wire version selects the suite;
there is no algorithm negotiation or downgrade path.

Pairwise messages avoid distributing one workspace payload key and permit
offline durable storage without a long-lived send nonce counter. A gateway
encrypts a separate snapshot/result for each current authorized recipient.
[Noise](https://noiseprotocol.org/noise.html) transport would require durable
handshake/counter/reordering state and bounded record fragmentation.
[MLS](https://www.rfc-editor.org/rfc/rfc9420.html) additionally provides group
forward secrecy and post-compromise recovery, with group-epoch and ratchet-state
integration. Those properties are outside this single-shot format. HPKE is
selected for the request/response gateway boundary and native interoperability,
not as a substitute for an MLS group protocol.

## Exact byte framing

`frame(domain, fields)` is `ASCII(domain) || 00`, followed by each field as
`u32BE(byte_length) || exact_bytes`. String fields are exact ASCII/UTF-8 bytes;
all identifiers are 1..128 ASCII letters, digits or `-_.:`. Numeric fields are
eight-byte unsigned big endian values. A byte string is framed after base64url
decoding. There is no JSON canonicalization or signing of JSON source text.
Every JSON struct rejects unknown fields.

`DevicePublic` has `device_id`, `role` (`computer` or `phone`),
`key_generation`, `signing_public_key` (32 bytes), and
`encryption_public_key` (32 bytes). Its frame domain is
`zerus-e2ee-device-v1`; fields follow that exact order.

`EncryptedEnvelope` has `version:1`, `header`, `enc` (32 bytes), `ciphertext`
(including the 16-byte GCM tag), and `signature` (64 bytes). Its header frame
domain is `zerus-e2ee-header-v1`, with fields in this order:

1. `workspace_id`
2. `sender_device_id`
3. `recipient_device_id`
4. `sender_key_generation`
5. `recipient_key_generation`
6. `gateway_id`
7. `route_id` (opaque destination, never a session name or path)
8. `request_id`
9. `kind`: `request`, `result`, `snapshot`, or `membership`
10. `operation_class`: `read`, `mutation`, or `control`
11. `expires_at` (Unix seconds)
12. `membership_revision`
13. `membership_head` (32 bytes)

The header frame is HPKE `info`. The signature frame domain is
`zerus-e2ee-envelope-v1`, with fields header frame, raw `enc`, raw ciphertext.
The stable envelope digest is SHA-256 of the
`zerus-e2ee-envelope-digest-v1` frame containing signature-frame bytes and raw
signature. Payload plaintext is at most 29 MiB; transport limits must also
account for base64url and framing overhead.

## Linear membership

`SignedMembership` has `version:1`, `statement`, and `signature`.
The statement frame domain is `zerus-e2ee-membership-v1`, with fields
`workspace_id`, `revision`, `previous_head` (32 bytes), `issuer_device_id`,
`issuer_key_generation`, `action` (`add` or `revoke`), `device` frame or empty
bytes, and `revoked_device_id` or empty bytes. The unused JSON payload is null.
The head is SHA-256 of the `zerus-e2ee-membership-head-v1` frame containing the
statement frame and its raw signature.

Genesis is revision zero with a zero previous head and a self-signed Add of
the founding computer. Every following statement must name the exact previous
head and revision plus one, and its issuer must be an active computer in that
previous state. Device IDs and key bindings are immutable; revocation permanently
tombstones the ID. Re-enrollment and rotation use a new device ID. Any active
computer can enroll or remove another member, including the founder. Previously
admitted members remain members after their inviter leaves. Self-removal of
the last computer is valid and leaves a retired workspace with no automatic
recovery path. Historical phone content and local drafts are unaffected.

The honest relay uses compare-and-swap to serialize changes. Stale proposals
must reconcile against signed history; endpoints must not automatically merge
or replace their pinned branch. Commit state, the history row and the
revision-to-head index atomically before using or acknowledging the change.
Persist an outgoing signed proposal before delivery; an uncertain delivery must
not cause another conflicting proposal or a replacement operation identity.

The relay can withhold data, partition endpoints, or present competing branches
to disconnected peers. Endpoints reject rollback and divergence from their
already pinned history. This format does not provide distributed consensus,
global fork prevention, or guaranteed availability under a malicious relay.
Authenticated peer communication can reveal competing heads, which must require
explicit repair; never silently choose the relay's branch.

Old envelopes may use a verified historical head from the same pinned branch.
Obtain that head from the endpoint's durable indexed history at the exact
revision; an unverified server-provided hash is not evidence. Check that both
sender and recipient remain current members with the exact immutable keys.
A revoked sender remains unauthorized even for its historically valid payload.

`MembershipState` contains `workspace_id`, `revision`, `head`, a `members` map
keyed by device ID, and sorted `revoked_device_ids`. The canonical state frame
domain is `zerus-e2ee-membership-state-v1`: workspace, revision, head, member
count, each member frame in ascending ASCII device-ID order, revoked count,
each revoked ID in ascending order. Membership is bounded to 4096 total active
and tombstoned device IDs; histories must use bounded indexed pages in storage.

## Invitation and enrollment

An `Invitation` is copied directly from a trusted computer to the joining
device. It has `version:1`, `workspace_id`, `invitation_id`, `purpose`, `issuer`
(`DevicePublic`), `expires_at`, `membership_revision`, `membership_head`, and
a freshly generated 32-byte `secret`. The validated relay HTTPS origin belongs
to the outer connection code. Neither the secret nor a secret verifier goes to
the relay. Phones and computers have distinct purposes. The copied code pins
the inviter and bootstrap head; no permanently online founding authority is
required. Creating/parsing a code performs no network mutation.

`EnrollmentCandidate` has `version:1`, `workspace_id`, `invitation_id`,
`purpose`, `device`, `proof`, and `signature`. The proof is HMAC-SHA256 with
the invitation secret over the `zerus-e2ee-enrollment-proof-v1` frame containing
workspace, invitation, purpose, and device frame. The candidate signature
domain is `zerus-e2ee-enrollment-candidate-v1`, with fields proof frame and raw
proof. Its digest is SHA-256 of the `zerus-e2ee-candidate-digest-v1` frame
containing candidate-signature frame and raw candidate signature.

The joining device durably persists its identity, candidate and pending
enrollment identity before sending. The online inviter verifies the HMAC,
candidate signature, exact purpose and expiry against its privately retained
invitation, confirms it is still an active computer, and atomically consumes
the one-time invitation with the exact accepted candidate. Public proof
verification alone cannot authorize another candidate's keys.

`SignedEnrollment` has `version:1`, `invitation_id`, `candidate_digest`,
`membership` (the signed Add), and `signature`. Its signature domain is
`zerus-e2ee-enrollment-approval-v1`, with fields invitation, raw candidate digest,
membership statement frame, and membership signature. This binds the invitation,
exact candidate keys and admitted membership transition. Server-issued bearer
credentials do not replace any of these endpoint checks.

`SignedMembershipSnapshot` has `version:1`, `state`, `issuer_device_id`,
`issuer_key_generation`, and `signature`. Its signature domain is
`zerus-e2ee-membership-snapshot-v1`, with fields state frame, issuer ID and key
generation. A new device accepts it only against the inviter key and exact
revision/head pinned in its copied invitation. It then verifies subsequent
history entries through its own Add. An existing endpoint must never replace
its pinned state with a snapshot. Bootstrap state and the approval may be public
membership metadata; they contain no private payload or invitation secret.

## Persistence, execution and limits

`IdentityRecord` is private storage: `version:1`, `device`,
`signing_private_key` (32-byte Ed25519 seed), and `encryption_private_key`
(32-byte raw X25519 scalar). Reload verifies both derived public keys. Keep it
inside platform-protected private storage. Secret-bearing Rust types have no
Debug implementation and wipe their owned secret buffers on drop; adapters must
also protect serializer buffers, logs and crash reports. Workspace switching
keeps identities, pins, pending drafts and journals in separate workspace
namespaces and must not disturb native sessions.

Persist the exact outgoing envelope before first submission and retry those
exact bytes. Re-encryption creates a different envelope, even for identical
plaintext, and is never an implicit retry after an uncertain delivery. Fresh
HPKE contexts avoid a persisted nonce counter but still require secure OS
randomness; RNG failure must fail closed.

`open` authenticates and decrypts against exact caller expectations. Its caller
must additionally enforce authenticated expiry, current membership, expected
gateway/direction/route and decrypted native run/conversation/question identities.
Cryptography intentionally does not reject an exact replay. Permanently journal
every encrypted request identity before invoking native code, including requests
whose visible operation class says read. An authorized sender can mislabel a
mutation. Relay retention hints must never cause it to execute twice. Keep
existing v1 hashes/receipts and native uncertainty protections unchanged.

HPKE is not forward secret against recipient private-key compromise. A stolen
recipient key exposes recorded ciphertext addressed to that key; a stolen
signing key permits sender impersonation until revocation is accepted. There
is no post-compromise recovery claim. Revocation stops accepted endpoints from
authorizing the removed device and from encrypting new content to it; it cannot
erase plaintext already received or force a partitioned endpoint to learn a
withheld update. The relay still sees the allowlisted header, ciphertext size,
timing, routing, membership and delivery/service metadata, and can deny service.

## Verification

Run `cargo +1.99.1 test --manifest-path services/mobile/crypto/Cargo.toml` and
`cargo +1.99.1 clippy --manifest-path services/mobile/crypto/Cargo.toml --all-targets -- -D warnings`.
`tests/vectors/protocol-v1.json` contains explicitly synthetic public fixture
private keys, the complete enrollment/bootstrap flow, all canonical byte frames
and an interoperable ciphertext. `rfc9180.json` is an independent published
suite vector. `rejected-keys-v1.json` contains standard small-order/noncanonical
Ed25519 encodings for native parity. Ignored fixture-generation utilities are
explicit maintenance commands, not skipped behavior tests.
