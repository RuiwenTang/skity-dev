# Copyright 2021 The Lynx Authors. All rights reserved.
# Licensed under the Apache License Version 2.0 that can be found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.15.0)

project(Skity VERSION 1.0.0)

# Compatible for MSVC runtime library
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

if(POLICY CMP0079)
  cmake_policy(SET CMP0079 NEW)
endif()

set(SKITY_VERSION 0.1.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  add_definitions(-DSKITY_RELEASE)
endif()

if (${SKITY_OPTIMIZE_O3})
  message("build with o3")
  set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG")
endif()

set(SKITY_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

set(CMAKE_CXX_EXTENSIONS OFF) # disable -std=gnu++11 by default

# options should set before define skity library
include(cmake/Options.cmake)

if (${SKITY_UNIFIED_LIB})
  if (EMSCRIPTEN)
    # hiding the legacy C++ API relies on the shared library's dynamic export
    # table; a static archive keeps every definition reachable to the linker,
    # which would let wrapper inline calls bind to the legacy implementations
    message(FATAL_ERROR "SKITY_UNIFIED_LIB requires a shared libskity (EMSCRIPTEN builds STATIC)")
  endif()
  if (${SKITY_GENERATE_FRAMEWORK})
    message(FATAL_ERROR "SKITY_UNIFIED_LIB cannot be combined with SKITY_GENERATE_FRAMEWORK (static framework)")
  endif()
endif()

if (MSVC AND SKITY_TEST AND NOT SKITY_UNIFIED_LIB)
  # MSVC needs to set export all symbols for test since UT case needs to access private symbols
  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON CACHE BOOL "Force MSVC export all symbols")
endif()

# define library
if(EMSCRIPTEN)
  add_library(skity STATIC)
else()
  add_library(skity SHARED)

  # binary size optimization
  if (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    if (ANDROID)
      target_link_options(skity PUBLIC "-flto")
      target_link_options(skity PUBLIC "-Wl,--exclude-libs,ALL" "-Wl,--gc-sections" "-fuse-ld=lld")
      target_link_options(skity PRIVATE "-Wl,--icf=safe")
      if (NOT (${SKITY_OPTIMIZE_O3}))
        target_compile_options(skity PUBLIC "-Oz")
      endif()
      target_compile_options(skity PUBLIC
        "-flto"
        "-fomit-frame-pointer"
        "-fno-sanitize=safe-stack"
        "-fno-strict-aliasing"
      )
    endif()
  endif()

  if (CMAKE_BUILD_TYPE STREQUAL "Release")
    # generate link map file
    if (ANDROID)
      target_link_options(skity PUBLIC
        "-Wl,-Map,${CMAKE_BINARY_DIR}/libskity.so.map"
        # make sure the link map file format can be parsed by supersize
      )
    elseif(${CMAKE_SYSTEM_NAME} STREQUAL "iOS")
      target_link_options(skity PUBLIC
        "LINKER:-map,${CMAKE_BINARY_DIR}/libskity.so.map"
      )
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      # using Clang
      target_link_options(skity PUBLIC
        "LINKER:-Map,${CMAKE_BINARY_DIR}/libskity.so.map"
      )
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      # using GCC
      target_link_options(skity PUBLIC
        "LINKER:-Map,${CMAKE_BINARY_DIR}/libskity.so.map"
      )
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
      target_link_options(skity PUBLIC
        "LINKER:-map,${CMAKE_BINARY_DIR}/libskity.so.map"
      )
    endif()
  endif()
endif()

# public header include
target_include_directories(skity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(skity PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
set_property(TARGET skity PROPERTY CXX_STANDARD 17)

target_include_directories(
  skity
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_PREFIX}/include>
)

target_compile_features(skity PUBLIC cxx_std_17)

add_library(skity::skity ALIAS skity)

# default config
include(GNUInstallDirs)

# platform
include(cmake/Platform.cmake)

# C-ABI-only export policy (SKITY_UNIFIED_LIB)
include(cmake/CABIExports.cmake)

# third party deps
include(cmake/ThirdPartyDep.cmake)

# add wgx module into build process
add_subdirectory(module/wgx)

# macro for wgx
target_compile_definitions(skity PRIVATE -DSKITY_WGX=1)

if(${SKITY_LOG})
  # no exception
  target_compile_definitions(skity PRIVATE -DSPDLOG_NO_EXCEPTIONS)
  target_compile_definitions(skity PRIVATE -DSKITY_LOG)
  # FIXME: disable -Wdeprecated-declarations warnings
  if (MSVC)
    target_compile_options(skity PRIVATE /wd4996)
  else()
    target_compile_options(skity PRIVATE -Wno-deprecated-declarations)
  endif()

  if(ANDROID)
    target_link_libraries(skity PRIVATE log)
  endif()

  if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    find_library(hilog-lib hilog_ndk.z)
    target_link_libraries(skity PRIVATE ${hilog-lib})
  endif()
endif()

if(${SKITY_VK_BACKEND})
  target_compile_definitions(skity PUBLIC -DSKITY_VULKAN)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(skity PUBLIC -DSKITY_VK_DEBUG_RUNTIME)
  endif()
endif()

if(${SKITY_GL_BACKEND})
  target_compile_definitions(skity PUBLIC -DSKITY_OPENGL)
endif()

if(${SKITY_MTL_BACKEND})
  target_compile_definitions(skity PRIVATE -DSKITY_METAL)
endif()

if(${SKITY_SW_RENDERER})
  target_compile_definitions(skity PUBLIC -DSKITY_CPU)
endif()

# Highway-backed SIMD accelerator for the software renderer. Kept as its own
# static library so size-sensitive packagings can exclude it entirely; the
# renderer falls back to the scalar path when it is absent. Only the three
# Highway runtime sources are compiled (targets/per_target/abort); the
# optional utility objects (timer, stats, profiler, ...) are not needed.
if(${SKITY_SW_ACCEL})
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/highway/hwy/highway.h")
    add_library(skity_sw_accel STATIC
      third_party/highway/hwy/targets.cc
      third_party/highway/hwy/per_target.cc
      third_party/highway/hwy/abort.cc
      src/render/sw/accel/sw_accel.cpp
    )
    target_include_directories(skity_sw_accel
      PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/highway
    )
    set_target_properties(skity_sw_accel PROPERTIES
      CXX_STANDARD 17
      POSITION_INDEPENDENT_CODE ON
    )
    # Keep the Highway namespace internal to the library; only skity entry
    # points should remain visible in dynamic symbol tables.
    if(NOT MSVC)
      target_compile_options(skity_sw_accel PRIVATE -fvisibility=hidden)
    endif()
    if(${SKITY_UNIFIED_LIB})
      # Same export policy as the skity target in the unified shape: only
      # the skity_* C ABI may be exported. SW_ACCEL_API reuses SKITY_API,
      # which must expand to nothing here as well, otherwise MSVC unified
      # builds export the two C++ runtime-switch symbols from libskity.dll
      # (GCC unified builds are already covered by the version script).
      target_compile_definitions(skity_sw_accel PRIVATE SKITY_HIDE_CXX_API=1)
    endif()
    if(MSVC)
      # Disable the AVX2 target copy on MSVC. The multi-target TU must be
      # compiled at the baseline ISA (MSVC has no per-function target
      # switching, unlike the GCC/Clang pragmas Highway uses there), and in
      # that mode MSVC models vector state at 128 bits: the AVX2 kernels
      # park every 256-bit value on the stack as two 16B halves and reload
      # it as one 32B vector. Such a load cannot be store-forwarded, so
      # every reconstitution stalls ~15-20 cycles, and the SrcOver engine
      # does it three times per 8-px iteration. Measured on a Core Ultra 7
      # 255H: the AVX2 copy runs 0.5 Gpx/s where the SSE4 copy of the same
      # source runs 3.6 (/arch:AVX2 would reach 5.6, but cannot be applied
      # to this TU without leaking VEX instructions into the SSE copies).
      # Dispatch falls back to SSE4 instead; revisit once the AVX2 kernels
      # can live in a separately compiled TU.
      # HWY_AVX2 = 1 << 9 = 512; target-wide so the dispatch tables and
      # hwy::SupportedTargets() (targets.cc) stay consistent.
      target_compile_definitions(skity_sw_accel PRIVATE HWY_DISABLED_TARGETS=512)
    endif()
    target_link_libraries(skity PRIVATE skity_sw_accel)
    target_compile_definitions(skity PUBLIC -DSKITY_SW_ACCEL=1)
  else()
    message(STATUS
      "SKITY_SW_ACCEL is ON but third_party/highway is missing; "
      "disable the option or sync habitat dependencies. Building without "
      "the accelerator.")
    set(SKITY_SW_ACCEL OFF CACHE BOOL "option for the Highway-backed SW accelerator" FORCE)
  endif()
endif()

target_compile_definitions(skity PRIVATE -DDISABLE_SKITY_EXPERIMENTAL_WARNINGS)

# Shared C-ABI foundation for module-owned C surfaces: the handle
# machinery (module/capi/common/handle.hpp) plus the base skity/c headers
# (handle macro, result codes, shared value structs) that the codec / io
# C headers build on. Defined here so it exists before any module
# subdirectory is configured. common/ is the only capi directory other
# modules may include from — capi's src/ stays private to it.
add_library(skity-capi-internal INTERFACE)
target_include_directories(skity-capi-internal
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/module/capi/common
    ${CMAKE_CURRENT_SOURCE_DIR}/module/capi/include)

# The C ABI library must be configured before codec / io: their internals
# call the skity_* entry points, which live in the standalone skity-capi
# library the modules link against (in the unified shape skity::capi is an
# alias of skity itself and the entry points compile into libskity).
if(${SKITY_CAPI_MODULE})
  add_subdirectory(module/capi)
endif()

if(${SKITY_CODEC_MODULE})
  add_subdirectory(module/codec)
endif()

if(${SKITY_IO_MODULE})
  add_subdirectory(module/io)
endif()


# header files
add_subdirectory(include)

# source files
add_subdirectory(src)

# source group for Xcode and Visual Studio
get_target_property(SKITY_SRC skity SOURCES)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/.. FILES ${SKITY_SRC})

if(${SKITY_EXAMPLE})
  add_subdirectory(example)
endif()

if(${SKITY_ENABLE_FONT_HARNESS})
  add_subdirectory(harness/font)
endif()

if(${SKITY_ENABLE_WEB_HARNESS})
  add_subdirectory(harness/web)
endif()

if(${SKITY_TEST})
  add_subdirectory(test)
endif()

if(${SKITY_BUILD_INSTALL})

  # package config file
  include(CMakePackageConfigHelpers)
  configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/skityConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/skity
  )

  # version
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/skityConfigVersion.cmake
    VERSION ${SKITY_VERSION}
    COMPATIBILITY SameMajorVersion
  )

  install(
    FILES
    ${CMAKE_CURRENT_BINARY_DIR}/skityConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/skityConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/skity
  )

  # install library
  install(
    TARGETS skity EXPORT skity-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  # install config file
  install(
    EXPORT skity-targets
    FILE skityTargets.cmake
    NAMESPACE skity::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/skity
  )
endif()

# build frameowrk for iOS and macOS
if(${SKITY_GENERATE_FRAMEWORK})
  add_subdirectory(platform/darwin)
endif()

if(${SKITY_CMAKE_TO_GN})
  # check if define SKITY_GN_REBASE_PATH
  if(NOT DEFINED SKITY_GN_REBASE_PATH)
    set(SKITY_GN_REBASE_PATH ".")
  endif()

  # generate gn files for skity and skity-codec
  include(cmake/CMakeToGN.cmake)

  # GN consumers build libskity as one library that carries the C ABI (the
  # layout the unified shape has anyway), so the standalone skity-capi of the
  # normal shape is folded into skity.gni instead of getting a gni of its own.
  # codec and io reach the core through those skity_* entry points, so without
  # this the GN build would have nothing that defines them.
  if (TARGET skity-capi)
    cmake_to_gni(skity MERGE skity-capi)
  else()
    cmake_to_gni(skity)
  endif()

  if (${SKITY_CODEC_MODULE})
    cmake_to_gni(skity-codec)
  endif()
endif()
