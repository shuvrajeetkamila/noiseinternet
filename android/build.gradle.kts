// NOISE INTERNET - Android module build.
//
// NEVER RUN: there is no JDK, Android SDK or NDK in the sandbox where this project was authored, so
// nothing here has been resolved, downloaded or compiled. Versions below are the ones the code assumes;
// if your SDK differs, change them - and expect the first build to need fixes, because this has never
// been through a compiler (see BUILD.md).
plugins {
    id("com.android.application") version "8.5.2" apply false
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
}
