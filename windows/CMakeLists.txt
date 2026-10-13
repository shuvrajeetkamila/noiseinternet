# Windows build of the NOISE INTERNET core + NoiseInternet.exe
#
# NEVER BUILT: the environment this project was written in has no MSVC, no MinGW-w64 and no zig, so
# this file has never been executed. It is written to be run, not to claim a result. If it fails to
# configure or compile, that is a bug to fix on a Windows machine, not something to work around here.
cmake_minimum_required(VERSION 3.16)
project(NoiseInternetWindows CXX)

if(NOT WIN32)
  message(FATAL_ERROR
    "windows/CMakeLists.txt targets Windows only (WASAPI). On Linux build the shared core with "
    "scripts/build_local.sh instead.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/W3 /EHsc /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

set(NI_CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../shared/acoustic_core)

file(GLOB NI_CORE_SRC ${NI_CORE_DIR}/src/*.cpp)
list(REMOVE_ITEM NI_CORE_SRC ${NI_CORE_DIR}/src/cli.cpp)   # the CLI is built separately, see below

add_library(ni_core STATIC ${NI_CORE_SRC})
target_include_directories(ni_core PUBLIC ${NI_CORE_DIR}/include)
target_include_directories(ni_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
# NI_WITH_WASAPI is what makes ni_audio_enumerate() and the pump in audio_wasapi.cpp real; without it
# the core reports "no audio backend" and the app refuses to claim anything.
target_compile_definitions(ni_core PUBLIC NI_WITH_WASAPI=1)
# proxy.cpp uses Winsock, and it is part of the shared core, so the socket library belongs here rather
# than in the app target. Without it the link fails on unresolved WSA*/getaddrinfo symbols. (Found by
# CI: this file had never been configured before the build-artifacts workflow ran.)
if(MSVC)
  target_compile_definitions(ni_core PUBLIC _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()
target_link_libraries(ni_core PUBLIC ws2_32)

add_executable(NoiseInternet
  ${CMAKE_CURRENT_LIST_DIR}/src/NoiseInternet/main.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/NoiseInternet/audio_wasapi.cpp)
target_link_libraries(NoiseInternet PRIVATE ni_core)
# ole32: COM + CoTaskMemFree.  uuid: the __uuidof/IID symbols.  ksuser: KSDATAFORMAT_* subtypes.
target_link_libraries(NoiseInternet PRIVATE ole32 uuid ksuser)

# The CLI too, so the same machine can run `ni selftest` / `ni render` / `ni decode` against the same
# core it just shipped in the GUI-less app.
add_executable(ni ${NI_CORE_DIR}/src/cli.cpp)
target_link_libraries(ni PRIVATE ni_core)

enable_testing()
file(GLOB NI_TEST_SRC ${NI_CORE_DIR}/tests/t_*.cpp)
foreach(t ${NI_TEST_SRC})
  get_filename_component(name ${t} NAME_WE)
  add_executable(${name} ${t})
  target_include_directories(${name} PRIVATE ${NI_CORE_DIR}/tests)
  target_link_libraries(${name} PRIVATE ni_core)
  add_test(NAME ${name} COMMAND ${name})
  # the vector files live in the repository, not next to the build
  target_compile_definitions(${name} PRIVATE
    NI_VECTOR_DIR="${NI_CORE_DIR}/../../protocol/protocol_vectors")
endforeach()
