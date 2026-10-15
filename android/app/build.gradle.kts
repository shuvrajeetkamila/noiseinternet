plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    // The manifest still carries `package=`; AGP 8 wants the namespace here instead.
    namespace = "com.noiseinternet.app"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "com.noiseinternet.app"
        // Both of these are visible in the app: versionName on the Android app-info screen, and the stamp
        // below inside the report itself. "Did not work" could not be answered while nobody could tell
        // whether the phone was running the build that had just been published (round 6). CI passes the
        // run number so every published build has a higher versionCode than the last, which also means
        // Android will update the app rather than refuse the install.
        versionCode = (System.getenv("NI_VERSION_CODE") ?: "6").toIntOrNull() ?: 6
        versionName = (System.getenv("NI_VERSION_NAME") ?: "0.6-debug")
        resValue("string", "ni_build_stamp", (System.getenv("NI_BUILD_STAMP") ?: "local build (no CI stamp)"))
        // 23: AudioRecord/AudioTrack setPreferredDevice and getDevices(); the native core itself is
        // plain C++17 and needs nothing from a specific API level.
        minSdk = 23
        targetSdk = 34
        ndk {
            // The shared core has no architecture-specific code (no NEON intrinsics anywhere), so the
            // ABI list is about test devices, not about portability.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared", "-DCMAKE_BUILD_TYPE=RelWithDebInfo")
                // The core's own stamp travels to the panel through the status JSON, so the app shows two
                // build strings: this one (the C++ engine) and the APK's own resValue above.
                arguments += listOf("-DNI_BUILD_STAMP=${System.getenv("NI_BUILD_STAMP") ?: "local build (no CI stamp)"}")
                cppFlags += listOf("-std=c++17", "-Wall", "-Wextra", "-fno-strict-aliasing")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/jni/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    signingConfigs {
        // A COMMITTED DEBUG KEY, on purpose.
        //
        // Until round 6 every CI run let the Android Gradle plugin generate `~/.android/debug.keystore` on
        // the runner, which means a brand-new random keypair per build. Android refuses to install an APK
        // whose signature differs from the installed app ("App not installed" / "package conflicts"), so
        // each published APK could only be installed after uninstalling the previous one - and the user
        // reported "did not work" with no way to tell whether that, or the app, was the problem.
        //
        // This key is a *debug* key and is deliberately public: it signs nothing but debug builds of an
        // app that is not on any store, and its whole purpose is that build N+1 installs over build N.
        // It must never be used for a release build: see the release block below and NOTICE-ci-debug-key.md.
        create("ciDebug") {
            storeFile = file("../ci-debug.p12")
            storeType = "PKCS12"
            storePassword = "noiseinternet"
            keyAlias = "noiseinternetci"
            keyPassword = "noiseinternet"
        }
    }

    buildTypes {
        debug {
            // Signed with the committed CI debug key when it is present (it is in this repository), so
            // successive builds install over each other. That is still a *local* signature: it is not a
            // release signature and this app is not published anywhere.
            isMinifyEnabled = false
            val ci = signingConfigs.findByName("ciDebug")
            if (ci?.storeFile?.exists() == true) signingConfig = ci
        }
        release {
            isMinifyEnabled = false
            // No signingConfig on purpose: a checked-in release key would be worse than none. Create
            // your own with keytool and point a local (uncommitted) keystore.properties at it.
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }

    lint {
        // A lint failure is fine, a lint warning silently ignored is not: keep the report.
        abortOnError = true
        warningsAsErrors = false
    }
}

// Deliberately no AndroidX, no Muun/Netty, no BouncyCastle: the crypto and the DSP live in the shared
// C++ core, and every dependency added here would be another license and another thing to audit for a
// project whose whole point is a small verifiable stack.
