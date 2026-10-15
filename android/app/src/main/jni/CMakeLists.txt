# CMake entry used by the Android Gradle plugin (externalNativeBuild). NEVER EXECUTED: no NDK exists
# in the sandbox where this was written. It compiles the *same* sources as the Linux and Windows builds;
# the only Android-specific file is the JNI bridge.
cmake_minimum_required(VERSION 3.22.1)
project(noiseinternet_jni CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# From android/app/src/main/jni up to the repository root is five levels.
get_filename_component(NI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../" ABSOLUTE)
set(NI_CORE_DIR "${NI_ROOT}/shared/acoustic_core")

if(NOT EXISTS "${NI_CORE_DIR}/include/ni/abi.h")
  message(FATAL_ERROR
    "shared/acoustic_core not found at ${NI_CORE_DIR} - the android module must live inside the "
    "repository, because it compiles the shared core from source rather than linking a prebuilt "
    "library (no prebuilt binary is shipped, and none could be trusted).")
endif()

file(GLOB NI_CORE_SRC "${NI_CORE_DIR}/src/*.cpp")
# cli.cpp has its own main() and is the Linux/Windows tool, not part of the mobile library.
list(REMOVE_ITEM NI_CORE_SRC "${NI_CORE_DIR}/src/cli.cpp")
# The simulator is a developer tool; it pulls in the channel model, which the app does not need.
list(REMOVE_ITEM NI_CORE_SRC "${NI_CORE_DIR}/src/sim.cpp")

add_library(noiseinternet_jni SHARED
  ${NI_CORE_SRC}
  "${CMAKE_CURRENT_SOURCE_DIR}/noiseinternet_jni.cpp")

target_include_directories(noiseinternet_jni PRIVATE
  "${NI_CORE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}")

# No NI_WITH_WASAPI here: on Android the PCM is produced and consumed in Kotlin (AudioRecord/AudioTrack)
# and pushed through ni_abi_feed_pcm / ni_abi_pull_pcm. There is deliberately no native AAudio backend
# in this tree - writing one would duplicate a data path that has never been compared against the Java
# one, and the point of the C ABI is that both platforms share one core.
# Which build of the engine this is (see NI_BUILD_STAMP in common.h). Gradle passes it from the CI
# environment; it reaches the phone's screen through the status JSON, so "am I running the new build?"
# is answerable by looking at the app instead of guessing.
if(NOT DEFINED NI_BUILD_STAMP)
  set(NI_BUILD_STAMP "local build (no CI stamp)")
endif()
target_compile_definitions(noiseinternet_jni PRIVATE NI_BUILD_STAMP="${NI_BUILD_STAMP}")

target_compile_options(noiseinternet_jni PRIVATE -Wall -Wextra -fvisibility=hidden)
target_link_libraries(noiseinternet_jni PRIVATE log m)
