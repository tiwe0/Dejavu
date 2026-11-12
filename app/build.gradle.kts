import java.util.Properties
import org.gradle.api.tasks.compile.JavaCompile
import org.gradle.api.tasks.JavaExec
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.tasks.OutputDirectory

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

abstract class ModuleAssetsTask : JavaExec() {
    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty
}

val modulePropertiesFile = rootProject.file("zygisk/module/module.prop")
val moduleProperties = Properties().apply {
    modulePropertiesFile.reader(Charsets.UTF_8).use { load(it) }
}
val dejavuModuleVersion = requireNotNull(moduleProperties.getProperty("version"))
val dejavuModuleVersionCode = requireNotNull(moduleProperties.getProperty("versionCode")).toInt()
val moduleZip = providers.gradleProperty("dejavuModuleZip").map { rootProject.file(it) }.orElse(
    rootProject.layout.projectDirectory.file("zygisk/dist/dejavu-zygisk-${dejavuModuleVersion}-arm64.zip").asFile,
)
val moduleAssetsDirectory = layout.buildDirectory.dir("generated/dejavuModuleAssets")
val compileDejavuModuleBundleTool by tasks.registering(JavaCompile::class) {
    source(file("scripts/ModuleBundleTool.java"))
    classpath = files()
    destinationDirectory.set(layout.buildDirectory.dir("moduleBundleTool"))
    options.release.set(17)
}
val generateDejavuModuleAssets by tasks.registering(ModuleAssetsTask::class) {
    dependsOn(compileDejavuModuleBundleTool)
    classpath = files(compileDejavuModuleBundleTool.flatMap { it.destinationDirectory })
    mainClass.set("ModuleBundleTool")
    inputs.file(modulePropertiesFile)
    inputs.property("moduleZipPath", moduleZip.map { it.absolutePath })
    // Let the tool report a useful packaging command if the exact ZIP is absent.
    inputs.files(moduleZip.map { if (it.isFile) listOf(it) else emptyList() })
    outputDirectory.set(moduleAssetsDirectory)
    doFirst {
        setArgs(listOf(moduleZip.get().absolutePath, modulePropertiesFile.absolutePath,
            moduleAssetsDirectory.get().asFile.absolutePath))
    }
}

val keystorePropertiesFile = rootProject.file("app/keystore.properties")
val keystoreProperties = Properties().apply {
    if (keystorePropertiesFile.isFile) {
        keystorePropertiesFile.inputStream().use { load(it) }
    }
}
val signingEnvironmentNames = listOf(
    "DEJAVU_SIGNING_KEYSTORE_FILE", "DEJAVU_SIGNING_STORE_PASSWORD",
    "DEJAVU_SIGNING_KEY_ALIAS", "DEJAVU_SIGNING_KEY_PASSWORD",
)
val signingEnvironment = signingEnvironmentNames.map { providers.environmentVariable(it).orNull }
val signingValues = if (signingEnvironment.any { !it.isNullOrEmpty() }) {
    require(signingEnvironment.all { !it.isNullOrEmpty() }) {
        "Incomplete Dejavu release signing environment; set all four DEJAVU_SIGNING_* values"
    }
    signingEnvironment.map { requireNotNull(it) }
} else if (keystorePropertiesFile.isFile) {
    listOf("storeFile", "storePassword", "keyAlias", "keyPassword").map { key ->
        requireNotNull(keystoreProperties.getProperty(key)?.takeIf { it.isNotEmpty() }) {
            "Incomplete app/keystore.properties: missing $key"
        }
    }
} else null
val releaseSigningReady = signingValues != null
signingValues?.let { values ->
    require(rootProject.file(values[0]).isFile) { "Dejavu release signing keystore does not exist" }
}
if (providers.gradleProperty("dejavuRequireReleaseSigning").orNull == "true") {
    require(releaseSigningReady) { "A stable release signing key is required for this build" }
}

android {
    namespace = "cafe.ivory.dejavu"
    compileSdk = 36

    defaultConfig {
        applicationId = "cafe.ivory.dejavu"
        minSdk = 26
        targetSdk = 36
        versionCode = dejavuModuleVersionCode
        versionName = dejavuModuleVersion.removePrefix("v")
        manifestPlaceholders["dejavuAppLabel"] = "@string/app_name"
    }

    signingConfigs {
        signingValues?.let { values ->
            create("release") {
                storeFile = rootProject.file(values[0])
                storePassword = values[1]
                keyAlias = values[2]
                keyPassword = values[3]
            }
        }
    }

    buildTypes {
        debug {
            if (providers.gradleProperty("dejavuSideBySide").orNull == "true") {
                applicationIdSuffix = ".debug"
                versionNameSuffix = "-dev"
                manifestPlaceholders["dejavuAppLabel"] = "dejavu Dev"
                signingConfig = signingConfigs.getByName("debug").apply {
                    storeFile = rootProject.file("out/tooling/android-user/debug.keystore")
                }
            }
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            if (releaseSigningReady) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    buildFeatures {
        compose = true
    }

    bundle {
        language {
            enableSplit = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }
}

androidComponents.onVariants { variant ->
    variant.sources.assets?.addGeneratedSourceDirectory(generateDejavuModuleAssets) { it.outputDirectory }
}
tasks.named("preBuild").configure { dependsOn(generateDejavuModuleAssets) }
tasks.matching { it.name in setOf("assembleRelease", "packageRelease", "bundleRelease", "packageReleaseBundle") }
    .configureEach {
        doFirst {
            check(releaseSigningReady) { "Release APKs require stable signing; configure DEJAVU_SIGNING_* or app/keystore.properties" }
        }
    }

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2025.05.00")
    implementation(composeBom)
    androidTestImplementation(composeBom)

    implementation("androidx.core:core-ktx:1.16.0")
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.9.0")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.9.0")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")

    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    debugImplementation("androidx.compose.ui:ui-tooling")
}
