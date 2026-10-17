import com.android.build.api.artifact.SingleArtifact
import groovy.json.JsonSlurper
import org.gradle.api.tasks.InputFile
import org.gradle.api.tasks.OutputFile
import org.gradle.api.file.RegularFileProperty
import org.gradle.api.tasks.TaskAction
import org.gradle.process.ExecOperations
import javax.inject.Inject
import org.jetbrains.kotlin.gradle.tasks.KotlinCompile
import org.gradle.api.tasks.compile.JavaCompile
import java.security.MessageDigest

plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

group = "androidx.camera"
version = "1.6.2-zerus.968a36ce"

val upstreamSources by configurations.creating { isTransitive = false; isCanBeConsumed = false }
val upstreamAar by configurations.creating { isTransitive = false; isCanBeConsumed = false }
val generated = layout.buildDirectory.dir("generated/upstream")
val prepareUpstream by tasks.registering(Exec::class) {
    inputs.files(upstreamSources, upstreamAar, file("provenance.json"), file("upstream.module.json"), file("prepare.py"), file("NOTICE.txt"), fileTree("patches"))
    outputs.dir(generated)
    doFirst {
        commandLine("python3", file("prepare.py"), "prepare", "--sources", upstreamSources.singleFile,
            "--aar", upstreamAar.singleFile, "--output", generated.get().asFile)
    }
}

android {
    namespace = "androidx.camera.video"
    compileSdk = 36
    defaultConfig { minSdk = 23; consumerProguardFiles(generated.map { it.file("consumer-rules.pro") }) }
    sourceSets.getByName("main") {
        java.setSrcDirs(listOf(generated.map { it.dir("java") }))
        res.setSrcDirs(listOf(generated.map { it.dir("res") }))
        manifest.srcFile(generated.map { it.file("AndroidManifest.xml") })
        resources.srcDir(generated.map { it.dir("resources") })
    }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_1_8; targetCompatibility = JavaVersion.VERSION_1_8 }
    kotlinOptions { jvmTarget = "1.8"; moduleName = "camera-video"; freeCompilerArgs += "-Xjvm-default=all" }
    testOptions.unitTests.isIncludeAndroidResources = true
    lint { checkReleaseBuilds = false }
}
tasks.named("preBuild") { dependsOn(prepareUpstream) }
tasks.withType<KotlinCompile>().configureEach {
    if (!name.contains("UnitTest")) compilerOptions.moduleName.set("camera-video")
}
@Suppress("UNCHECKED_CAST")
val sourcePins = JsonSlurper().parse(file("provenance.json")) as Map<String, Any>
val processorPin = sourcePins["auto_value_processor_sha256"] as String
tasks.withType<JavaCompile>().configureEach {
    if (!name.contains("UnitTest")) {
        inputs.property("autoValueProcessorSha256", processorPin)
        doFirst {
            val processors = checkNotNull(options.annotationProcessorPath).files
            check(processors.size == 1 && processors.single().name == "auto-value-1.6.3.jar") {
                "Review any change to the pinned CameraX annotation processor classpath"
            }
            val digest = MessageDigest.getInstance("SHA-256").digest(processors.single().readBytes())
                .joinToString("") { "%02x".format(it) }
            check(digest == processorPin) { "CameraX AutoValue processor digest mismatch" }
        }
    }
}

@Suppress("UNCHECKED_CAST")
val upstream = JsonSlurper().parse(file("upstream.module.json")) as Map<String, Any>
@Suppress("UNCHECKED_CAST")
val variants = upstream["variants"] as List<Map<String, Any>>
@Suppress("UNCHECKED_CAST")
fun variantItems(name: String, key: String) = variants.single { it["name"] == name }[key] as List<Map<String, Any>>
fun coordinate(item: Map<String, Any>): String {
    @Suppress("UNCHECKED_CAST") val version = item["version"] as Map<String, String>
    require(version.keys == setOf("requires")) { "Review richer upstream version constraints" }
    return "${item["group"]}:${item["module"]}:${version["requires"]}"
}
val apiDependencies = variantItems("releaseVariantReleaseApiPublication", "dependencies").map(::coordinate)
val runtimeDependencies = variantItems("releaseVariantReleaseRuntimePublication", "dependencies").map(::coordinate)
val atomicConstraints = variantItems("releaseVariantReleaseApiPublication", "dependencyConstraints")
require(apiDependencies.size == 5 && runtimeDependencies.size == 14 && atomicConstraints.size == 9)
require(atomicConstraints == variantItems("releaseVariantReleaseRuntimePublication", "dependencyConstraints"))
dependencies {
    upstreamSources("androidx.camera:camera-video:1.6.2:sources@jar")
    upstreamAar("androidx.camera:camera-video:1.6.2@aar")
    apiDependencies.forEach { add("api", it) }
    runtimeDependencies.filterNot { it in apiDependencies }.forEach { add("implementation", it) }
    constraints { atomicConstraints.forEach { item -> add("api", coordinate(item)) { because(item["reason"].toString()) } } }
    annotationProcessor("com.google.auto.value:auto-value:1.6.3")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.15.1")
    testImplementation("org.mockito:mockito-core:5.17.0")
    testImplementation("androidx.test:core:1.6.1")
    testImplementation("com.google.truth:truth:1.4.4")
}

abstract class RetainUpstreamMetadata : DefaultTask() {
    @get:Inject abstract val execOperations: ExecOperations
    @get:InputFile abstract val rebuilt: RegularFileProperty
    @get:InputFile abstract val original: RegularFileProperty
    @get:InputFile abstract val script: RegularFileProperty
    @get:OutputFile abstract val output: RegularFileProperty
    @TaskAction fun repack() {
        execOperations.exec { commandLine("python3", script.get().asFile, "repack", "--original", original.get().asFile,
            "--rebuilt", rebuilt.get().asFile, "--output", output.get().asFile) }
    }
}
androidComponents.onVariants { variant ->
    val retain = tasks.register<RetainUpstreamMetadata>("retain${variant.name.replaceFirstChar { it.uppercaseChar() }}UpstreamMetadata") {
        original.fileProvider(upstreamAar.elements.map { it.single().asFile })
        script.set(layout.projectDirectory.file("prepare.py"))
    }
    variant.artifacts.use(retain).wiredWithFiles(RetainUpstreamMetadata::rebuilt, RetainUpstreamMetadata::output).toTransform(SingleArtifact.AAR)
    val verify = tasks.register<Exec>("verify${variant.name.replaceFirstChar { it.uppercaseChar() }}UpstreamArtifact") {
        val aar = variant.artifacts.get(SingleArtifact.AAR)
        val report = layout.buildDirectory.file("reports/upstream-compatibility/${variant.name}.json")
        inputs.file(aar)
        inputs.files(upstreamAar, file("verify.py"))
        outputs.file(report)
        doFirst { commandLine("python3", file("verify.py"), "--original", upstreamAar.singleFile,
            "--rebuilt", aar.get().asFile, "--report", report.get().asFile) }
    }
    tasks.matching { it.name == "test${variant.name.replaceFirstChar { it.uppercaseChar() }}UnitTest" ||
        it.name == "lint${variant.name.replaceFirstChar { it.uppercaseChar() }}" }.configureEach { dependsOn(verify) }
}
