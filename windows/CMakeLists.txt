# Windows build of the NOISE INTERNET core + NoiseInternet.exe
#
# This file IS executed - by CI (.github/workflows/build.yml, windows job) with MSVC 19.51 on
# windows-latest, which is also where every compile error in this project's Windows half was found:
# the development sandbox has no MSVC, MinGW-w64 or zig, so nothing here was ever run locally. The
# measured state of the build is in artifacts/README.md, not asserted here.
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
  # Standalone exe. The default MSVC CRT is the DLL one (/MD), which makes NoiseInternet.exe depend on
  # vcruntime140.dll, vcruntime140_1.dll and msvcp140.dll being present on the machine - on a clean
  # Windows install they are not, and a loose .exe copied out of a zip has no installer to put them
  # there. /MT links the CRT into the binary instead, so the artifact needs nothing beside it.
  # Turn it off (-DNI_STATIC_RUNTIME=OFF) to get the smaller /MD build that does need the redist.
  option(NI_STATIC_RUNTIME "link the MSVC runtime statically (/MT) so the exe is standalone" ON)
  if(NI_STATIC_RUNTIME)
    # The flag-variable trick below is INERT on its own since CMake 3.15: under policy CMP0091=NEW the
    # runtime is not in CMAKE_CXX_FLAGS_RELEASE at all, it is chosen by CMAKE_MSVC_RUNTIME_LIBRARY, whose
    # default is MultiThreadedDLL. First attempt at a standalone exe shipped /MD anyway, and the only
    # reason anybody noticed is that the CI packaging step reads the import table of the linked file
    # (dumpbin /dependents) instead of trusting this option - recorded as defect 28. Both mechanisms are
    # set so the option works whether or not the policy is NEW.
    if(POLICY CMP0091)
      set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    foreach(_v CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
      string(REPLACE "/MDd" "/MTd" ${_v} "${${_v}}")
    endforeach()
    foreach(_v CMAKE_C_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELEASE
               CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO
               CMAKE_C_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_MINSIZEREL)
      string(REPLACE "/MD" "/MT" ${_v} "${${_v}}")
    endforeach()
    message(STATUS "MSVC runtime: asking for static (/MT); CMAKE_MSVC_RUNTIME_LIBRARY=[${CMAKE_MSVC_RUNTIME_LIBRARY}] - the packaging step verifies the real import table")
  else()
    message(STATUS "MSVC runtime: dynamic (/MD) - the redist DLLs must ship next to the exe")
  endif()
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

# audio_wasapi.cpp is part of ni_core, not of the app: the tests (t_abi calls ni_audio_enumerate,
# which resolves to ni_wasapi_enumerate when NI_WITH_WASAPI is on) link the same library, and a
# backend that only the GUI links would leave those tests unresolvable. Found by CI, first MSVC run.
target_sources(ni_core PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/NoiseInternet/audio_wasapi.cpp)
target_link_libraries(ni_core PUBLIC ole32 uuid ksuser propsys)

add_executable(NoiseInternet
  ${CMAKE_CURRENT_LIST_DIR}/src/NoiseInternet/main.cpp)
target_link_libraries(NoiseInternet PRIVATE ni_core)
# ole32: COM + CoTaskMemFree.  uuid: the __uuidof/IID symbols.  ksuser: KSDATAFORMAT_* subtypes.
target_link_libraries(NoiseInternet PRIVATE ole32 uuid ksuser)

# The CLI too, so the same machine can run `ni selftest` / `ni render` / `ni decode` against the same
# core it just shipped in the GUI-less app.
add_executable(ni ${NI_CORE_DIR}/src/cli.cpp)
# `ni demo` spawns curl; that path is POSIX-only and is compiled out here (see cli.cpp).
target_compile_definitions(ni PRIVATE NI_NO_DEMO_SPAWN=1)
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
