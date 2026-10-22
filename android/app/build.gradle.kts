// MobileCAD XR for Meta Quest 3 (and other Android OpenXR headsets): the whole MobileCAD app from the mobilecad/ submodule
// (UI, engine bridge, catalog, icons; its packaged FreeCAD runtime via engine/package_runtime.py) plus the XR core.
import java.util.Properties
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}
val mobilecad = rootDir.resolve("../mobilecad")
val keyProps = Properties().apply {
    val f = file(System.getProperty("user.home") + "/Android/keys/mobilecad-release.properties"); if (f.exists()) f.inputStream().use { load(it) } }
android {
    namespace = "app.mobilecad"          // MobileCAD's sources use app.mobilecad.R; the store id below is separate
    compileSdk = 34
    defaultConfig {
        applicationId = "app.mobilecad.xr"
        minSdk = 32                      // Meta Horizon OS store guidance: min 32, target 34
        targetSdk = 34
        versionCode = ((System.currentTimeMillis() / 60000L) - 29454720L).toInt()
        versionName = "0.9.4"
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
    // MobileCAD's app (its own manifest and launcher activity are not used: XrActivity hosts its UI on the panel)
    sourceSets["main"].java.srcDir(mobilecad.resolve("app/app/src/main/java"))
    sourceSets["main"].res.srcDir(mobilecad.resolve("app/app/src/main/res"))
    sourceSets["main"].assets.srcDir(mobilecad.resolve("app/app/src/main/assets"))
    sourceSets["main"].assets.srcDir(rootProject.file("../assets"))      // the hand / controller models
    buildFeatures { prefab = true; compose = true; buildConfig = false }
    composeOptions { kotlinCompilerExtensionVersion = "1.5.14" }
    signingConfigs {
        if (keyProps.containsKey("storeFile")) create("release") {
            storeFile = file(keyProps.getProperty("storeFile")); storePassword = keyProps.getProperty("storePassword")
            keyAlias = keyProps.getProperty("keyAlias"); keyPassword = keyProps.getProperty("keyPassword")
        }
    }
    buildTypes { getByName("release") { isMinifyEnabled = false; signingConfigs.findByName("release")?.let { signingConfig = it } } }
    packaging { jniLibs { pickFirsts += "**/libc++_shared.so"; useLegacyPackaging = true } }
    // the engine's Python (the mobilecad bridge, shims, vendored modules) always from the submodule: the staged runtime in
    // src/<flavor>/assets only provides the native parts, which change rarely (engine/package_runtime.py stages both)
    // aapt's default pattern drops asset directories starting with "_" (zipfile/_path, _pyrepl, Python packages ...)
    androidResources { ignoreAssetsPattern = "!.svn:!.git:!.ds_store:!*.scc:!CVS:!thumbs.db:!picasa.ini:!*~" }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
}
dependencies {
    implementation("org.khronos.openxr:openxr_loader_for_android:1.1.63")
    // as MobileCAD
    implementation(platform("androidx.compose:compose-bom:2024.09.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.activity:activity-compose:1.9.2")
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("com.caverock:androidsvg-aar:1.4")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation(files(mobilecad.resolve("app/app/libs/Qt6Android.jar")))
}

// see the comment in android {}: refresh the bridge Python in each flavor's staged runtime before its assets are merged
val syncEnginePython = tasks.register("syncEnginePython") {
    doLast {
        for (abi in listOf("arm64", "x86")) {
            val bridge = file("src/$abi/assets/freecad-runtime/bridge")
            if (!bridge.exists()) continue
            delete(bridge.resolve("mobilecad"))
            copy { from(mobilecad.resolve("engine/python/mobilecad")); into(bridge.resolve("mobilecad")); exclude("**/__pycache__/**") }
            for (d in listOf("shims", "vendor")) copy { from(mobilecad.resolve("engine/python/$d")); into(bridge); exclude("**/__pycache__/**") }
        }
    }
}
tasks.named("preBuild") { dependsOn(syncEnginePython) }
