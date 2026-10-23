import java.util.Properties
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
val stableVersionName = "0.4.0"
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
android {
    namespace = "app.zerus.mobile"
    compileSdk = 36
    defaultConfig {
        applicationId = "app.zerus.mobile"
        minSdk = 26
        targetSdk = 36
        versionCode = updateVersionCode ?: stableVersionCode
        versionName = previewVersionName ?: (devBuildNumber?.let { "$devBaseVersion-dev.$it" } ?: stableVersionName)
        buildConfigField("boolean", "FIREBASE_ENABLED", firebaseEnabled.toString())
        testInstrumentationRunner = providers.gradleProperty("zerusInstrumentationRunner").orNull ?: "androidx.test.runner.AndroidJUnitRunner"
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
    implementation("com.google.crypto.tink:tink-android:1.24.0")
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
