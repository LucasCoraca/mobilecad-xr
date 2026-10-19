// MobileCAD XR for Meta Quest 3 (and other Android OpenXR headsets). Reuses MobileCAD's FreeCAD engine from the
// mobilecad/ submodule: the Kotlin Engine, the JNI bridge (fcbridge) and the packaged runtime (engine/package_runtime.py).
import java.util.Properties
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}
val mobilecad = rootDir.resolve("../mobilecad")
val keyProps = Properties().apply {
    val f = file(System.getProperty("user.home") + "/Android/keys/mobilecad-release.properties"); if (f.exists()) f.inputStream().use { load(it) } }
// MobileCAD sources shared with the phone app, copied in at build time (Engine + the Qt host-activity hook)
val syncEngine by tasks.registering(Sync::class) {
    from(mobilecad.resolve("app/app/src/main/java")) { include("app/mobilecad/freecad/Engine.kt", "org/qtproject/qt/android/QtHostAccess.java") }
    into(layout.buildDirectory.dir("generated/mobilecad/java"))
}
android {
    namespace = "app.mobilecad.xr"
    compileSdk = 34
    defaultConfig {
        applicationId = "app.mobilecad.xr"
        minSdk = 32                      // Meta Horizon OS store guidance: min 32, target 34
        targetSdk = 34
        versionCode = ((System.currentTimeMillis() / 60000L) - 29454720L).toInt()
        versionName = "0.1"
        externalNativeBuild { cmake { arguments += listOf("-DANDROID_STL=c++_shared", "-DMOBILECAD_DIR=" + mobilecad.absolutePath) } }
    }
    ndkVersion = "29.0.14206865"
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.22.1" } }
    // arm64: Quest 3 (headset build, immersive). x86: Android emulator, where there is no OpenXR runtime -> 2D preview
    flavorDimensions += "abi"
    productFlavors {
        create("arm64") { dimension = "abi"; ndk { abiFilters += "arm64-v8a" } }
        create("x86") { dimension = "abi"; ndk { abiFilters += "x86_64" } }
    }
    sourceSets["main"].java.srcDir(layout.buildDirectory.dir("generated/mobilecad/java"))
    buildFeatures { prefab = true }
    signingConfigs {
        if (keyProps.containsKey("storeFile")) create("release") {
            storeFile = file(keyProps.getProperty("storeFile")); storePassword = keyProps.getProperty("storePassword")
            keyAlias = keyProps.getProperty("keyAlias"); keyPassword = keyProps.getProperty("keyPassword")
        }
    }
    buildTypes { getByName("release") { isMinifyEnabled = false; signingConfigs.findByName("release")?.let { signingConfig = it } } }
    packaging { jniLibs { pickFirsts += "**/libc++_shared.so"; useLegacyPackaging = true } }
    // aapt's default pattern drops asset directories starting with "_" (zipfile/_path, _pyrepl, Python packages ...)
    androidResources { ignoreAssetsPattern = "!.svn:!.git:!.ds_store:!*.scc:!CVS:!thumbs.db:!picasa.ini:!*~" }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
}
tasks.named("preBuild") { dependsOn(syncEngine) }
dependencies {
    implementation("org.khronos.openxr:openxr_loader_for_android:1.1.63")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation(files(mobilecad.resolve("app/app/libs/Qt6Android.jar")))
}
