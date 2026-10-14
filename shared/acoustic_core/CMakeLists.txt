# NOISE INTERNET — shared acoustic core
#
# One C++17 static library, one CLI, one test binary per area. The *same* file list is used
# by the Android NDK build (android/app/src/main/cpp/CMakeLists.txt includes this directory)
# and by the Windows build, so the protocol cannot drift between platforms.
cmake_minimum_required(VERSION 3.16)
project(noise_internet_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(NI_BUILD_TESTS "Build the automated test suite" ON)
option(NI_USE_LIBSODIUM "Use libsodium for AEAD/HMAC instead of the vendored implementation" OFF)
option(NI_BUILD_SHARED "Also build a shared library (used by the WASM and JNI shells)" OFF)

if(POLICY CMP0076)
  cmake_policy(SET CMP0076 NEW)
endif()

set(NI_SOURCES
  src/common.cpp
  src/crc.cpp
  src/bits.cpp
  src/fft.cpp
  src/error_correction.cpp
  src/frame_codec.cpp
  src/synchronization.cpp
  src/modem.cpp
  src/channel_estimator.cpp
  src/channel_sim.cpp
  src/crypto.cpp
  src/aec.cpp
  src/session.cpp
  src/arq.cpp
  src/wav.cpp
  src/capture_diagnosis.cpp
  src/proxy.cpp
  src/node.cpp
  src/sim.cpp
  src/abi.cpp
)

add_library(ni_core STATIC ${NI_SOURCES})
target_include_directories(ni_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(ni_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(NOT MSVC)
  target_compile_options(ni_core PRIVATE -fno-strict-aliasing)
  target_link_libraries(ni_core PUBLIC m)
endif()
if(WIN32)
  target_link_libraries(ni_core PUBLIC ws2_32 winmm ole32 propsys uuid)
endif()
if(ANDROID)
  target_link_libraries(ni_core PUBLIC log)
endif()
if(NI_USE_LIBSODIUM)
  find_library(SODIUM_LIB sodium)
  if(NOT SODIUM_LIB)
    message(FATAL_ERROR "NI_USE_LIBSODIUM=ON but libsodium was not found")
  endif()
  target_compile_definitions(ni_core PUBLIC NI_USE_LIBSODIUM=1)
  target_link_libraries(ni_core PUBLIC ${SODIUM_LIB})
endif()

if(NI_BUILD_SHARED)
  add_library(ni_core_shared SHARED ${NI_SOURCES})
  target_include_directories(ni_core_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  set_target_properties(ni_core_shared PROPERTIES OUTPUT_NAME noiseinternet_core)
endif()

# ------------------------------------------------------------------ CLI harness
add_executable(ni src/cli.cpp)
target_link_libraries(ni PRIVATE ni_core)

# ------------------------------------------------------------------ tests
if(NI_BUILD_TESTS)
  enable_testing()
  # The suite is exactly the test files that exist in tests/: dropping in a new t_foo.cpp registers
  # it, and configure never fails because the list named a file that is not there (a hard-coded
  # list did exactly that while a test was still being written).
  file(GLOB NI_TEST_SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_*.cpp)
  list(SORT NI_TEST_SOURCES)
  foreach(src ${NI_TEST_SOURCES})
    get_filename_component(t ${src} NAME_WE)
    add_executable(${t} tests/${src})
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    # so ctest finds the generated reference vectors whatever the working directory is
    target_compile_definitions(${t} PRIVATE
      NI_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../protocol/protocol_vectors")
    target_link_libraries(${t} PRIVATE ni_core)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()
