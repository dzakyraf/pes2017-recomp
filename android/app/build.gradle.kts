plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// The native build is the repository's own CMakeLists.txt, two folders up.
// Nothing about the runtime is duplicated here: the same project that makes
// the desktop executable makes libpes17.so, with ANDROID set (cmake/android.cmake).
val repositoryRoot = file("../..")

// What a build machine may set, so that a CI job does not need a patched copy
// of this file (.github/workflows/android.yml):
//
//   PES17_NDK_VERSION        which NDK to use, when the runner has another one
//   PES17_VERSION_NAME       the release this apk is, without the leading "v"
//                          ("0.0.1"); the workflow takes it from the tag
//   PES17_VERSION_CODE       the number Android compares to decide whether an apk
//                          is an update; the workflow sets it when it must
//   PES17_NATIVE_OPTIMISATION  -O2 compiles the recompiled tree a good deal
//                          quicker than -O3 and runs within a few percent of
//                          it; a runner with six hours cares about that
//   PES17_COMPILER_LAUNCHER  ccache, usually
//   PES17_XEX_SHA256_SP/MP   the hashes the installer checks a player's own
//                          copy against, for a build that has the recompiled
//                          sources but not the disc they came from
//   PES17_KEYSTORE and friends  a signing key, so the apk is installable
fun setting(name: String): String? = System.getenv(name)?.trim()?.ifEmpty { null }

// The retail default.xex the app installs: the Kotlin folder import checks it before copying 6 GB.
val pesXexSha256 = setting("PES17_XEX_SHA256_SP") ?: "b914ec9a1abb3a9b42a64255b6aa8ff27312da126f7c18f8c880106b832779e6"

// The app's own sources have to be here. They went missing once -- written,
// built against, and never committed -- and the result was an apk that
// installed, launched, and died on a manifest pointing at classes that did
// not exist. An empty source tree is not a warning, it is a broken build.
val kotlinSources = fileTree("src/main/java") { include("**/*.kt") }.files
if (kotlinSources.size < 10) {
    throw GradleException(
        "Only ${kotlinSources.size} Kotlin sources under android/app/src/main/java. " +
            "The app cannot be built from this tree -- check that they are committed " +
            "(git ls-files '*.kt')."
    )
}

android {
    namespace = "com.pes2017.recomp"
    compileSdk = 35
    // r27+ aligns a library for 16 KB pages by default and clang is new enough.
    ndkVersion = setting("PES17_NDK_VERSION") ?: "28.2.13676358"

    defaultConfig {
        applicationId = "com.pes2017.recomp"
        // Android 8.0. AAudio, ASurfaceControl-free presenting, and the
        // Vulkan 1.1 drivers this renderer needs all predate it; below that
        // there is no device with the memory to run this anyway.
        minSdk = 26
        targetSdk = 35
        // Bump EVERY published release: Android refuses to install an update
        // whose versionCode is at or below the installed one. CI may set
        // PES17_VERSION_CODE instead of editing this file.
        // 16: v0.0.2-alpha, over 15 (v0.0.2), and v0.0.1, where the names started
        // again: the code never goes back, or an installed copy would refuse it.
        // 17: v0.1.0. 18: v0.1.1.
        versionCode = setting("PES17_VERSION_CODE")?.toIntOrNull() ?: 18
        // Which build this is, carried inside the apk. "The wrong apk" has
        // cost more than one evening, and every time the only way to settle
        // it was to guess. The launcher shows this and Android's app info
        // shows it, so the file on the phone can be matched against the run
        // that made it without downloading anything twice.
        // The release the build is from, which is what a report has to say to
        // be acted on. It was "1.0" -- a number no release ever had.
        versionName = "${setting("PES17_VERSION_NAME") ?: "0.0.1"} " +
            "(${setting("PES17_BUILD_SHA") ?: "local"})"
        buildConfigField("String", "PES_XEX_SHA256", "\"$pesXexSha256\"")
        buildConfigField("String", "BUILD_SHA", "\"${setting("PES17_BUILD_SHA") ?: "local"}\"")
        buildConfigField("String", "BUILD_STAMP", "\"${setting("PES17_BUILD_STAMP") ?: "unstamped"}\"")

        ndk {
            // The guest's address space is 4 GiB reserved in one mapping, so
            // a 32-bit process cannot host it at all.
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    // The runtime's optional parts, all off: they cost frames
                    // and the phone has none to spare.
                    "-DPES17_DIAGNOSTICS=OFF",
                    "-DPES17_USE_SDL=OFF",
                    "-DPES17_LOGGING=ON",
                    "-DCMAKE_BUILD_TYPE=Release",
                )
                setting("PES17_COMPILER_LAUNCHER")?.let {
                    arguments += listOf(
                        "-DCMAKE_C_COMPILER_LAUNCHER=$it",
                        "-DCMAKE_CXX_COMPILER_LAUNCHER=$it",
                    )
                }
                val sp = setting("PES17_XEX_SHA256_SP") ?: "b914ec9a1abb3a9b42a64255b6aa8ff27312da126f7c18f8c880106b832779e6"
                val mp = setting("PES17_XEX_SHA256_MP") ?: "b914ec9a1abb3a9b42a64255b6aa8ff27312da126f7c18f8c880106b832779e6"
                arguments += listOf("-DPES17_XEX_SHA256_SP=$sp", "-DPES17_XEX_SHA256_MP=$mp")
                cppFlags += (setting("PES17_NATIVE_OPTIMISATION") ?: "-O3")
            }
        }
    }

    // One executable, one app: PES 2017 is installed from the disc as it is.
    defaultConfig {
        resValue("string", "app_name", "PES 2017")
    }

    // -PprebuiltNative=<dir with arm64-v8a/*.so> packages libraries built by
    // ./build.sh instead of compiling the 1,600 generated units a second time here.
    val prebuiltNative = providers.gradleProperty("prebuiltNative").orNull
    if (prebuiltNative == null) {
        externalNativeBuild {
            cmake {
                path = File(repositoryRoot, "CMakeLists.txt")
                version = "3.22.1+"
            }
        }
    } else {
        sourceSets.getByName("main").jniLibs.srcDir(prebuiltNative)
    }

    // A release apk with no key is not installable, and an app nobody can
    // install is not a build. A key given in the environment is used; without
    // one the debug key signs it, which Android accepts and which makes it
    // plain that this is not a store build.
    signingConfigs {
        val keystorePath = setting("PES17_KEYSTORE")
        if (keystorePath != null && file(keystorePath).exists()) {
            create("supplied") {
                storeFile = file(keystorePath)
                storePassword = setting("PES17_KEYSTORE_PASSWORD")
                keyAlias = setting("PES17_KEY_ALIAS")
                keyPassword = setting("PES17_KEY_PASSWORD") ?: setting("PES17_KEYSTORE_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.findByName("supplied")
                ?: signingConfigs.getByName("debug")
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // The library is enormous and the debug info larger still; it is
            // kept beside the build for reading crash reports against.
            ndk { debugSymbolLevel = "SYMBOL_TABLE" }
        }
        debug {
            isMinifyEnabled = false
            isJniDebuggable = true
        }
    }

    packaging {
        jniLibs {
            // The apk is installed extracted: the runtime dlopens the
            // adrenotools hook libraries by path, and a driver loaded from a
            // zip needs real files on disk beside them.
            useLegacyPackaging = true
        }
    }

    buildFeatures {
        buildConfig = true
        viewBinding = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation("androidx.documentfile:documentfile:1.0.1")
}
