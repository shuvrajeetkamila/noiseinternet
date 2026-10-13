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

    buildTypes {
        debug {
            // Signed with the Android debug keystore by the Gradle plugin. That is a *local* signature:
            // it lets `adb install` run this build. It is not a release signature, and nothing in this
            // repository claims otherwise - and on the machine where this code was written, no APK was
            // produced at all, so not even this applies until you build it yourself.
            isMinifyEnabled = false
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
