# CameraX persistent recording backport

This source-built Android library replaces **only** `androidx.camera:camera-video:1.6.2`
inside Zerus. It fixes stopping a persistent recording while its camera is being
rebound. It is not an official AndroidX release. No AAR, JAR or generated AutoValue
source is checked in.

`provenance.json` pins the official Google Maven source JAR, AAR and Gradle module
metadata by SHA-256. Gradle obtains the original source/AAR as isolated, non-transitive
build inputs, never as additional application implementations. `prepare.py` verifies
every input, applies the patch at exact original line offsets without fuzz, verifies
the patched Recorder digest and writes a complete original/compiled source map.
All other Java/Kotlin sources are unchanged. Original Apache-2.0 headers and LICENSE
are retained, and the modified Recorder and packaged notice identify the backport.
Python 3 and JDK 17 or 21 are required alongside the ordinary Android SDK 36 tools.
The Gradle wrapper, Android/Kotlin plugins and AutoValue processor versions are pinned.
Java/Kotlin target Java 8, as the original artifact does; the Kotlin module name stays
`camera-video` so mangled internal method names remain compatible.

The complete upstream fix is
[968a36cef8c813f2f3b2c5f1765d40a0836cdab1](https://github.com/androidx/androidx/commit/968a36cef8c813f2f3b2c5f1765d40a0836cdab1).
The 1.6.2 release predates its necessary future-lifetime prerequisites:

- [7a2969cb](https://github.com/androidx/androidx/commit/7a2969cbf9b3689d89c4a19117ea9378451c5319)
  keeps the audio encoding future across rebind and prevents cancellation propagation.
- [7b541c10](https://github.com/androidx/androidx/commit/7b541c103f7ff50274cd42f31b96d44d318aaf4b)
  completes a released video encoder's future, handles stale callbacks and finalizes
  encoder errors while stopping.

The patch retains all three fixes. Its compatibility adaptations preserve the
original persistent-recording predicate, Java 8 generic syntax and the package-local
`mEncodingFutures` field as the current non-cancellation-propagating aggregate view.
It neither catches the assertion nor waits an arbitrary amount before Stop.

## Artifact and dependency checks

The library compiles all official sources and regenerates AutoValue implementations.
The final AAR replaces only the official AAR's `classes.jar`; all original outer
entries, including manifest, resources, public resource list, annotations, consumer
ProGuard rules and license, retain their exact bytes. Inner JAR version/Kotlin
metadata is retained or reproduced, and the license/backport notice is packaged.
Archive timestamps, modes and order are canonical; stored ZIP entries avoid a zlib
version dependency. Clean rebuild reproducibility is checked using the recorded
compiler/toolchain identity.

`upstream.module.json` drives the original five API dependencies, fourteen runtime
dependencies and nine atomic CameraX constraints. Other CameraX modules stay 1.6.2.
The app's `verifyCameraVideoGraph` compares all four resolved debug/release compile
and runtime graphs against the official dependency. It preserves AGP's compile/runtime
alignment and normalizes only the replacement's project identity and its now-inapplicable
external version-alignment constraint. All other dependency and constraint edges must
match. It requires exactly one rebuilt implementation and no original `camera-video`
component. No test-only dependency becomes an application dependency.

`verifyDebugUpstreamArtifact` / `verifyReleaseUpstreamArtifact` require every original
class and generated AutoValue API, including package-local members, generic signatures,
constant values and declared exceptions to survive. Three source-retention annotation
classes that AndroidX strips when publishing and two exact Java 8 access bridges for
the new private completer may be present additionally. Original synthetic members,
including Kotlin default-argument APIs, remain compared. JDK 17 also emits four enum-switch
holders that JDK 21 omits for these same sources. The verifier permits only their exact
names and declarations: Java 8 synthetic classes extending Object, the explicitly named
synthetic static final int-array fields and a static initializer, with no other members
(including private members). Reports separate these holders from source annotations.
All original API and metadata checks apply on both JDKs. Reproducible AAR hashes are
specific to the recorded JDK; JDK 17 and 21 AARs need not have identical bytes. No other
additional class or consumer metadata change is accepted. These checks precede the
normal module unit-test and release-lint tasks.

## Qualification

From `mobile/android`, the existing hosted command runs both app and library tests:

```sh
./gradlew --no-daemon testDebugUnitTest lintRelease -PzerusFirebase=true
```

`RecorderRebindStopTest` retains the four upstream deterministic stop/rebind ordering
bodies and all their assertions, with fixture adapters for the older 1.6.2 APIs.
The fixtures additionally cover retained audio completion, Stop while unbound with
and without audio, stale callbacks, continued/paused recording and encoder error
while stopping. The real Recorder, SurfaceRequests, encoding futures and Finalize
callbacks execute under Robolectric with controlled fake encoders; there is no
hardware-readiness timer. `test-provenance.json` records the original fixture sources
and the narrow API adaptations. Test XML and compatibility reports live under this
module's `build/test-results` and `build/reports`; the scoped hosted failure collector
retains both. A negative control can put the exact pinned original class JAR first
on the **unit-test-only** classpath and set `zerus.camerax.expectedRecorderSha256`;
the fixture verifies that Recorder class digest before executing any ordering.
This does not alter an application artifact.

For a fresh, owned emulator with both synthetic cameras, build matching app/test APKs:

```sh
./gradlew --no-daemon :app:assembleDebug :app:assembleDebugAndroidTest \
  -PzerusInstrumentationRunner=app.zerus.mobile.CameraXRebindStopInstrumentation
```

Run `app.zerus.mobile.test/app.zerus.mobile.CameraXRebindStopInstrumentation` with
`am instrument -w` on that explicit emulator serial. It requires successful front-to-back
binding and immediately requests Stop, Cancel or lifecycle stop in the same main-loop
action. It checks final media/discard behavior, single completion, retired recorder
ownership, empty temporary storage and unchanged voice state. It never treats Preview
streaming, elapsed time or `bindToLifecycle` return as an encoder-ready fence. Initial
recording samples are awaited only before switching. The retained ordinary
`VideoMessageSmokeInstrumentation` remains a separate end-to-end local gesture and
no-replay regression with fixture-only HTTP responses.

Synthetic qualification does not prove physical-camera behavior or a successful
live ASR/video send. Integration, publication and maintained deployment remain separate.
