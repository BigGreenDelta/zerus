import java.util.Properties
import java.util.zip.ZipFile
import java.io.File

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}
val signingPropertiesPath = providers.gradleProperty("zerusSigningProperties").orNull
    ?: providers.environmentVariable("ZERUS_ANDROID_SIGNING_PROPERTIES").orNull
    ?: "${System.getProperty("user.home")}/.config/hgs/mobile/dev-signing/signing.properties"
val privateSigningFile = file(signingPropertiesPath)
val privateSigning = Properties().apply { if(privateSigningFile.isFile) privateSigningFile.inputStream().use { load(it) } }
val firebaseEnabled = providers.gradleProperty("zerusFirebase").orNull == "true"
val previewVersionName = providers.gradleProperty("zerusVersionName").orNull?.also {
    require(it.matches(Regex("[A-Za-z0-9][A-Za-z0-9.+-]{0,79}"))) { "Invalid Android preview version name." }
}
val stableVersionName = "0.2.0"
val stableVersionCode = rootProject.file("VERSION_CODE").readText().trim().toInt().also {
    require(it in 1..2_100_000_000) { "Invalid committed Android version code." }
}
val devBuildNumber = providers.gradleProperty("zerusDevBuildNumber").orNull?.toInt()?.also { require(it > 0) }
val updateVersionCode = providers.gradleProperty("zerusVersionCode").orNull?.toInt()?.also { require(it in 1..2_100_000_000) }
val devBaseVersion = rootProject.file("DEV_VERSION").readText().trim().also {
    require(it.matches(Regex("(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)")))
    val firstChange = it.split('.').map(String::toInt).zip(stableVersionName.split('.').map(String::toInt)).firstOrNull { pair -> pair.first != pair.second }
    require(firstChange != null && firstChange.first > firstChange.second) { "DEV_VERSION must be newer than stable." }
}
require(devBuildNumber == null || (updateVersionCode ?: 0) > stableVersionCode) {
    "A Dev build needs a versionCode newer than the committed Stable code."
}
if (firebaseEnabled) {
    require(file("google-services.json").isFile) { "Optional Firebase builds require a private app/google-services.json." }
    apply(plugin = "com.google.gms.google-services")
    apply(plugin = "com.google.firebase.crashlytics")
}
// The task replica: zerus-replica through UniFFI (crates/replica-ffi), built with the
// pinned CI toolchain. The host library gives the Kotlin bindings and is what the JVM
// unit tests load; cargo-ndk builds the library of each packaged ABI.
val rustToolchain = "1.99.1"
val replicaCrate = rootProject.file("../../crates/replica-ffi")
val replicaSources = listOf(replicaCrate, rootProject.file("../../crates/replica")).flatMap {
    listOf(it.resolve("src"), it.resolve("Cargo.toml"), it.resolve("Cargo.lock"))
} + replicaCrate.resolve("uniffi.toml")
val rustBuild = layout.buildDirectory.dir("rust")
val hostLibraryName = System.getProperty("os.name").let {
    when {
        it.startsWith("Mac") -> "libzerus_replica_ffi.dylib"
        it.startsWith("Windows") -> "zerus_replica_ffi.dll"
        else -> "libzerus_replica_ffi.so"
    }
}
// A missing tool fails with what to install (mobile/android/README.md), not with a process
// that could not start.
fun requireTool(need: String, vararg command: String): String {
    val output = runCatching {
        val process = ProcessBuilder(*command).redirectErrorStream(true).start()
        process.inputStream.bufferedReader().readText().takeIf { process.waitFor() == 0 }
    }.getOrNull()
    return output ?: throw GradleException("The task replica needs $need; see mobile/android/README.md, \"Build and install\".")
}
val hostLibrary = rustBuild.map { it.file("host/debug/$hostLibraryName") }
val uniffiKotlin = layout.buildDirectory.dir("generated/uniffi/kotlin")
val rustJniLibs = rustBuild.map { it.dir("jniLibs") }
// Each packaged ABI and the Rust target its library is built for.
val replicaAbis = mapOf("arm64-v8a" to "aarch64-linux-android", "armeabi-v7a" to "armv7-linux-androideabi", "x86_64" to "x86_64-linux-android")
val rustHostLibrary by tasks.registering(Exec::class) {
    inputs.files(replicaSources)
    outputs.file(hostLibrary)
    workingDir = replicaCrate
    doFirst { requireTool("Rust $rustToolchain (rustup toolchain install $rustToolchain)", "cargo", "+$rustToolchain", "--version") }
    commandLine("cargo", "+$rustToolchain", "build", "--locked", "--target-dir", rustBuild.get().dir("host").asFile.path)
}
val uniffiBindings by tasks.registering(Exec::class) {
    dependsOn(rustHostLibrary)
    inputs.file(hostLibrary)
    inputs.files(replicaSources)
    outputs.dir(uniffiKotlin)
    workingDir = replicaCrate
    doFirst { delete(uniffiKotlin) }
    commandLine("cargo", "+$rustToolchain", "run", "--locked", "--features", "bindgen", "--bin", "uniffi-bindgen",
        "--target-dir", rustBuild.get().dir("bindgen").asFile.path, "--", "generate",
        "--library", hostLibrary.get().asFile.path, "--language", "kotlin", "--no-format",
        "--out-dir", uniffiKotlin.get().asFile.path)
}

android {
    namespace = "app.zerus.mobile"
    compileSdk = 36
    ndkVersion = "28.2.13676358"
    defaultConfig {
        applicationId = "app.zerus.mobile"
        minSdk = 26
        targetSdk = 36
        versionCode = updateVersionCode ?: stableVersionCode
        versionName = previewVersionName ?: (devBuildNumber?.let { "$devBaseVersion-dev.$it" } ?: stableVersionName)
        buildConfigField("boolean", "FIREBASE_ENABLED", firebaseEnabled.toString())
        testInstrumentationRunner = providers.gradleProperty("zerusInstrumentationRunner").orNull ?: "androidx.test.runner.AndroidJUnitRunner"
        // The ABIs the task replica's library is built for (crates/replica-ffi). JNA's own
        // library would otherwise add ABIs on which the replica is missing.
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64") }
    }
    if(privateSigningFile.isFile) signingConfigs.create("development") {
        val keyFile = File(checkNotNull(privateSigning.getProperty("storeFile")))
        storeFile = if(keyFile.isAbsolute) keyFile else File(privateSigningFile.parentFile,keyFile.path)
        storePassword = checkNotNull(privateSigning.getProperty("storePassword"))
        keyAlias = checkNotNull(privateSigning.getProperty("keyAlias"))
        keyPassword = checkNotNull(privateSigning.getProperty("keyPassword"))
    }
    buildTypes {
        getByName("release") {
            isDebuggable = false
            isMinifyEnabled = true
            isShrinkResources = true
            if(privateSigningFile.isFile) signingConfig = signingConfigs.getByName("development")
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }
    buildFeatures { compose = true; buildConfig = true }
    sourceSets.getByName("main") {
        java.srcDir(uniffiKotlin)
        jniLibs.srcDir(rustJniLibs)
    }
    if (firebaseEnabled) {
        sourceSets.getByName("main").java.srcDir("src/firebase/java")
        sourceSets.getByName("debug").manifest.srcFile("src/firebase/AndroidManifest.xml")
        sourceSets.getByName("release").manifest.srcFile("src/firebase/AndroidManifest.xml")
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    packaging { resources.excludes += "/META-INF/{AL2.0,LGPL2.1}" }
    // The CI gate requires lintRelease, which also fails on the fatal vital checks.
    lint { checkReleaseBuilds = false }
}
val rustAndroidLibraries by tasks.registering(Exec::class) {
    inputs.files(replicaSources)
    outputs.dir(rustJniLibs)
    workingDir = replicaCrate
    val ndk = android.ndkDirectory
    doFirst {
        requireTool("cargo-ndk 4.1.2 (cargo install cargo-ndk --version 4.1.2 --locked)", "cargo", "+$rustToolchain", "ndk", "--version")
        if (!ndk.isDirectory) throw GradleException("The task replica needs NDK ${android.ndkVersion} (sdkmanager 'ndk;${android.ndkVersion}'); see mobile/android/README.md, \"Build and install\".")
        // Without rustup the targets cannot be listed; cargo then names what is missing.
        val installed = runCatching { requireTool("rustup", "rustup", "target", "list", "--installed", "--toolchain", rustToolchain) }.getOrNull()
        val missing = replicaAbis.values.filterNot { installed == null || installed.lines().contains(it) }
        if (missing.isNotEmpty()) throw GradleException("The task replica needs the Rust targets ${missing.joinToString()} (rustup target add --toolchain $rustToolchain ${missing.joinToString(" ")}); see mobile/android/README.md, \"Build and install\".")
    }
    environment("ANDROID_NDK_HOME", ndk.path)
    commandLine("cargo", "+$rustToolchain", "ndk", "-t", "arm64-v8a", "-t", "armeabi-v7a", "-t", "x86_64",
        "--platform", "26", "-o", rustJniLibs.get().asFile.path, "build", "--release", "--locked",
        "--target-dir", rustBuild.get().dir("android").asFile.path)
}
/** Collects the licence notices of every crate built into the library for each packaged ABI. */
abstract class RustLicenses : Exec() {
    @get:OutputDirectory abstract val outputDir: DirectoryProperty
}
// The notices for Third-party notices, as each variant's generated assets, so that whatever
// reads the assets runs the collector first; it fails the build when a crate ships none.
androidComponents {
    onVariants { variant ->
        val licenses = tasks.register<RustLicenses>("${variant.name}RustLicenses") {
            // The collector reads the crates cargo has downloaded for the host build.
            dependsOn(rustHostLibrary)
            val collector = rootProject.file("../../scripts/collect-licenses.py")
            inputs.file(collector)
            inputs.file(replicaCrate.resolve("Cargo.lock"))
            inputs.dir(rootProject.file("../../packaging/rust-licenses"))
            executable = "python3"
            doFirst { requireTool("python3", "python3", "--version") }
            argumentProviders.add(CommandLineArgumentProvider {
                listOf(collector.path, outputDir.get().dir("rust-licenses").asFile.path,
                    "--manifest-path", replicaCrate.resolve("Cargo.toml").path) +
                    replicaAbis.values.flatMap { listOf("--target", it) }
            })
            doFirst { project.delete(outputDir) }
        }
        variant.sources.assets?.addGeneratedSourceDirectory(licenses, RustLicenses::outputDir)
    }
}
// Every packaged ABI carries the replica's library and the APK the notices of its crates on
// all of them: a build that loses either fails instead of shipping without it.
val checkReplicaPackaging by tasks.registering {
    dependsOn("assembleDebug")
    val apk = layout.buildDirectory.file("outputs/apk/debug/app-debug.apk")
    doLast {
        ZipFile(apk.get().asFile).use { zip ->
            val entries: Set<String> = zip.entries().asSequence().map { entry -> entry.name }.toSet()
            val notices = listOf("assets/rust-licenses/inventory.json", "assets/licenses/jna.txt", "assets/licenses/libffi.txt")
            val missing = (replicaAbis.keys.map { "lib/$it/libzerus_replica_ffi.so" } + notices)
                .filterNot { it in entries }
            require(missing.isEmpty()) { "The debug APK lacks ${missing.joinToString()}" }
            val others = entries.filter { it.startsWith("lib/") }.map { it.split('/')[1] }.toSet() - replicaAbis.keys
            require(others.isEmpty()) { "The debug APK carries ABIs without the task replica: $others" }
            val inventory = zip.getInputStream(zip.getEntry("assets/rust-licenses/inventory.json")).bufferedReader().use { it.readText() }
            val targets = (groovy.json.JsonSlurper().parseText(inventory) as Map<*, *>)["targets"]
            require(targets == replicaAbis.values.toList()) { "The notices cover $targets, not ${replicaAbis.values}" }
        }
    }
}
tasks.named("preBuild") { dependsOn(uniffiBindings) }
tasks.matching { it.name.matches(Regex("merge.*JniLibFolders")) }.configureEach { dependsOn(rustAndroidLibraries) }
tasks.withType<Test>().configureEach {
    dependsOn(rustHostLibrary)
    systemProperty("uniffi.component.zerus_replica_ffi.libraryOverride", hostLibrary.get().asFile.path)
}
gradle.taskGraph.whenReady {
    if(allTasks.any { it.name in setOf("packageRelease","signReleaseBundle") })
        require(privateSigningFile.isFile) { "Distributable builds require private development signing configuration." }
}
// Replace every transitive camera-video edge too; never package both implementations.
configurations.configureEach {
    resolutionStrategy.dependencySubstitution {
        substitute(module("androidx.camera:camera-video")).using(project(":camera-video-fixed"))
    }
}
val verifyCameraVideoGraph by tasks.registering {
    val report = layout.buildDirectory.file("reports/camera-video-graph/resolved.json")
    outputs.file(report)
    outputs.upToDateWhen { false }
    doLast {
        val results = linkedMapOf<String, Any>()
        val names = listOf("debugRuntimeClasspath", "releaseRuntimeClasspath", "debugCompileClasspath", "releaseCompileClasspath")
        val baselines = names.associateWith { name ->
            val fixed = configurations.getByName(name)
            val originalDependencies = fixed.allDependencies.filterNot {
                it is ProjectDependency && it.dependencyProject.path == ":camera-video-fixed"
            } + dependencies.create("androidx.camera:camera-video:1.6.2")
            val original = configurations.detachedConfiguration(*originalDependencies.toTypedArray())
            fixed.attributes.keySet().forEach { key ->
                @Suppress("UNCHECKED_CAST") val typed = key as org.gradle.api.attributes.Attribute<Any>
                original.attributes.attribute(typed, checkNotNull(fixed.attributes.getAttribute(typed)))
            }
            original.dependencyConstraints.addAll(fixed.allDependencyConstraints)
            original
        }
        // AGP aligns each compile classpath with its runtime classpath. Preserve that policy in
        // the detached official baseline too, rather than comparing an unaligned synthetic graph.
        for (variant in listOf("debug", "release")) {
            baselines.getValue("${variant}CompileClasspath").shouldResolveConsistentlyWith(baselines.getValue("${variant}RuntimeClasspath"))
        }
        for (name in names) {
            val fixed = configurations.getByName(name)
            val original = baselines.getValue(name)
            fun componentId(id: org.gradle.api.artifacts.component.ComponentIdentifier): String = when (id) {
                is org.gradle.api.artifacts.component.ModuleComponentIdentifier -> "${id.group}:${id.module}:${id.version}"
                is org.gradle.api.artifacts.component.ProjectComponentIdentifier ->
                    if (id.projectPath == ":camera-video-fixed") "androidx.camera:camera-video:1.6.2" else "application"
                else -> error("Unreviewed component: $id")
            }
            fun graph(configuration: Configuration): Set<String> = configuration.incoming.resolutionResult.allComponents
                .flatMap { source -> source.dependencies.map { edge ->
                    check(edge is org.gradle.api.artifacts.result.ResolvedDependencyResult) { "Unresolved dependency: $edge" }
                    "${componentId(source.id)} -> ${componentId(edge.selected.id)}; constraint=${edge.isConstraint}"
                } }.toSortedSet()
            val selected = fixed.incoming.resolutionResult.allComponents
            check(selected.count { it.id is org.gradle.api.artifacts.component.ProjectComponentIdentifier &&
                (it.id as org.gradle.api.artifacts.component.ProjectComponentIdentifier).projectPath == ":camera-video-fixed" } == 1)
            check(selected.none { it.id is org.gradle.api.artifacts.component.ModuleComponentIdentifier &&
                (it.id as org.gradle.api.artifacts.component.ModuleComponentIdentifier).let { module ->
                    module.group == "androidx.camera" && module.module == "camera-video" } })
            val before = graph(original)
            val after = graph(fixed)
            // AGP adds an external version constraint when aligning compile to runtime. A pinned
            // project replacement has no external version to constrain; all nine upstream
            // published atomic constraints and all ordinary dependency edges remain compared.
            val externalPin = "application -> androidx.camera:camera-video:1.6.2; constraint=true"
            val normalizedBefore = before - externalPin
            val normalizedAfter = after - externalPin
            check(normalizedBefore == normalizedAfter) { "$name CameraX replacement changed graph: removed=${normalizedBefore - normalizedAfter}; added=${normalizedAfter - normalizedBefore}" }
            results[name] = linkedMapOf("equal_after_project_identity_normalization" to true,
                "original_external_alignment_pin" to (externalPin in before),
                "rebuilt_implementations" to 1, "original_implementations" to 0, "edges" to after.toList())
        }
        report.get().asFile.apply { parentFile.mkdirs();writeText(groovy.json.JsonOutput.prettyPrint(groovy.json.JsonOutput.toJson(results)) + "\n") }
    }
}
tasks.named("preBuild") { dependsOn(verifyCameraVideoGraph) }
dependencies {
    implementation(platform("androidx.compose:compose-bom:2025.06.01"))
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.9.1")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.9.1")
    implementation("androidx.work:work-runtime-ktx:2.10.2")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")
    implementation("androidx.camera:camera-camera2:1.6.2")
    implementation("androidx.camera:camera-lifecycle:1.6.2")
    implementation("androidx.camera:camera-view:1.6.2")
    implementation(project(":camera-video-fixed"))
    implementation("com.google.zxing:core:3.5.4")
    implementation("org.commonmark:commonmark:0.30.0")
    implementation("org.commonmark:commonmark-ext-gfm-tables:0.30.0")
    implementation("org.commonmark:commonmark-ext-gfm-strikethrough:0.30.0")
    implementation("org.commonmark:commonmark-ext-task-list-items:0.30.0")
    implementation("org.commonmark:commonmark-ext-autolink:0.30.0")
    constraints {
        implementation("org.nibor.autolink:autolink") {
            version { strictly("0.12.0") }
            because("Use the reviewed upstream version with bundled license notices.")
        }
    }
    debugImplementation("androidx.compose.ui:ui-tooling")
    // The task replica's bindings reach the Rust library through JNA.
    implementation("net.java.dev.jna:jna:5.17.0@aar")
    testImplementation("net.java.dev.jna:jna:5.17.0")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20250517")
    testImplementation("com.squareup.okhttp3:mockwebserver:4.12.0")
    if (firebaseEnabled) {
        implementation(platform("com.google.firebase:firebase-bom:34.0.0"))
        implementation("com.google.firebase:firebase-messaging")
        implementation("com.google.firebase:firebase-crashlytics")
        implementation("com.google.firebase:firebase-analytics")
        implementation("androidx.fragment:fragment:1.8.8")
    }
}
