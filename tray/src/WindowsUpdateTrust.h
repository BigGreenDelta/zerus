#pragma once
#include <QByteArray>
#include <QString>

// The trust boundary of Windows self-update for installer builds
// (docs/superpowers/specs/2026-10-18-windows-installer-design.md). A build runs a
// downloaded installer only when it carries both trust anchors: the update
// manifest's public key and the expected Authenticode publisher. Until the owner
// creates them (zerus-qxdx.1), every build is link-only and runs nothing it
// downloads.
namespace WindowsUpdateTrust {
// The ECDSA P-256 public key (X and Y, 32 bytes each) that signs the Windows update
// manifest; empty when this build has none.
QByteArray manifestKey();
// The publisher every installer must be Authenticode-signed by; empty when none.
QString expectedPublisher();
// Whether this build may ever run a downloaded installer: Windows with both anchors.
bool selfUpdateConfigured();
// Whether `signature` (r and s, 32 bytes each) is a valid ECDSA P-256 SHA-256
// signature of exactly `data` by `publicKey`. Windows CNG only: false elsewhere.
bool verifyManifest(const QByteArray &data,const QByteArray &signature,const QByteArray &publicKey);
// The signer's display name of a valid Authenticode signature of the file at `path`,
// with revocation checked for the whole chain; empty when the file is unsigned, the
// signature or chain is not trusted, or elsewhere than Windows. Revocation checks may
// wait on the network, so never call it on the GUI thread.
QString authenticodePublisher(const QString &path);
}
