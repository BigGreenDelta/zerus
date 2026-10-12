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
val stableVersionName = "0.1.30"
val devBuildNumber = providers.gradleProperty("zerusDevBuildNumber").orNull?.toInt()?.also { require(it > 0) }
val updateVersionCode = providers.gradleProperty("zerusVersionCode").orNull?.toInt()?.also { require(it in 1..2_100_000_000) }
val devBaseVersion = rootProject.file("DEV_VERSION").readText().trim().also {
    require(it.matches(Regex("(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)")))
    val firstChange = it.split('.').map(String::toInt).zip(stableVersionName.split('.').map(String::toInt)).firstOrNull { pair -> pair.first != pair.second }
    require(firstChange != null && firstChange.first > firstChange.second) { "DEV_VERSION must be newer than stable." }
}
require(devBuildNumber == null || updateVersionCode != null) { "A Dev build needs an explicit increasing versionCode." }
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
        versionCode = updateVersionCode ?: 31
        versionName = devBuildNumber?.let { "$devBaseVersion-dev.$it" } ?: stableVersionName
        buildConfigField("boolean", "FIREBASE_ENABLED", firebaseEnabled.toString())
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
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
}
gradle.taskGraph.whenReady {
    if(allTasks.any { it.name in setOf("packageRelease","signReleaseBundle") })
        require(privateSigningFile.isFile) { "Distributable builds require private development signing configuration." }
}
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
