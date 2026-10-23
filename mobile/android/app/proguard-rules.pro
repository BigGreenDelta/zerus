# Optional Firebase entry point is called reflectively only in private Firebase builds.
-keep class app.zerus.mobile.OptionalFirebase { public static *; }

# Tink 1.24.0's optional X-Wing implementation uses API 37 / ART extension 22
# classes absent from compileSdk 36. WorkspaceCrypto fixes the KEM to X25519,
# so it never selects that guarded X-Wing path.
-dontwarn android.crypto.hpke.Hpke
-dontwarn android.crypto.hpke.Message
