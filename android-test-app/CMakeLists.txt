cmake_minimum_required(VERSION 3.22.1)
project(WebRtcAndroidTest)

# =============================================================================
# Android cross-compilation setup (mirrors superbuild logic)
# =============================================================================
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    # Auto-detect OpenSSL cross-compile target from ANDROID_ABI
    if(NOT BUILD_OPENSSL_PLATFORM OR BUILD_OPENSSL_PLATFORM STREQUAL "")
        if(ANDROID_ABI STREQUAL "arm64-v8a")
            set(BUILD_OPENSSL_PLATFORM "android-arm64" CACHE STRING "" FORCE)
        elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
            set(BUILD_OPENSSL_PLATFORM "android-arm" CACHE STRING "" FORCE)
        elseif(ANDROID_ABI STREQUAL "x86_64")
            set(BUILD_OPENSSL_PLATFORM "android-x86_64" CACHE STRING "" FORCE)
        elseif(ANDROID_ABI STREQUAL "x86")
            set(BUILD_OPENSSL_PLATFORM "android-x86" CACHE STRING "" FORCE)
        endif()
    endif()

    # OpenSSL's Configure needs ANDROID_NDK_HOME and the NDK clang on PATH
    if(CMAKE_ANDROID_NDK)
        set(ENV{ANDROID_NDK_HOME} "${CMAKE_ANDROID_NDK}")
    elseif(DEFINED ENV{ANDROID_NDK})
        set(ENV{ANDROID_NDK_HOME} "$ENV{ANDROID_NDK}")
    endif()
    file(GLOB _ndk_bins "$ENV{ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/*/bin")
    list(GET _ndk_bins 0 _ndk_bin)
    if(_ndk_bin)
        set(ENV{PATH} "${_ndk_bin}:$ENV{PATH}")
    endif()
endif()

# =============================================================================
# Deps install prefix — keep per-binary-dir to avoid ABI clobbering
# =============================================================================
set(OPEN_SRC_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/deps-install" CACHE PATH "" FORCE)

# The NDK legacy toolchain sets FIND_ROOT_PATH_MODE to ONLY, which prevents
# find_package(OpenSSL/GTest) from finding self-built libraries at OPEN_SRC_INSTALL_PREFIX.
# Two-pronged fix:
# 1. Add OPEN_SRC_INSTALL_PREFIX to CMAKE_FIND_ROOT_PATH so cmake searches it even in ONLY mode.
# 2. Also override modes to BOTH as a belt-and-suspenders fallback.
list(APPEND CMAKE_FIND_ROOT_PATH "${OPEN_SRC_INSTALL_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH CACHE STRING "" FORCE)

# =============================================================================
# SDK sub-build
# =============================================================================
set(BUILD_SAMPLE            OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS       ON  CACHE BOOL "" FORCE)
set(BUILD_TEST              ON  CACHE BOOL "" FORCE)
set(ENABLE_SELF_CONTAINED   ON  CACHE BOOL "" FORCE)  # vendors kvspic, forces ENABLE_SIGNALING=OFF
set(ENABLE_AWS_SDK_IN_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. sdk)

# =============================================================================
# JNI test library  (webrtc_test_jni.so)
# =============================================================================
set(WEBRTC_TST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tst")
file(GLOB JNI_TEST_SOURCES "${WEBRTC_TST_DIR}/*.cpp")

# Exclude main.cpp — jni_bridge.cpp is the entry point for the JNI library
list(FILTER JNI_TEST_SOURCES EXCLUDE REGEX ".*main\\.cpp$")

# Exclude signaling tests (ENABLE_SIGNALING=OFF, headers/symbols unavailable)
list(FILTER JNI_TEST_SOURCES EXCLUDE REGEX ".*Signaling.*\\.cpp")
list(FILTER JNI_TEST_SOURCES EXCLUDE REGEX ".*IngestionFunctionalityTests\\.cpp")
list(FILTER JNI_TEST_SOURCES EXCLUDE REGEX ".*CustomEndpointTest\\.cpp")
list(FILTER JNI_TEST_SOURCES EXCLUDE REGEX ".*DualStackEndpointsTest\\.cpp")
list(FILTER JNI_TEST_SOURCES EXCLUDE REGEX ".*TurnConnectionFunctionalityTest\\.cpp")
list(FILTER JNI_TEST_SOURCES EXCLUDE REGEX ".*IceConfigParsingTest\\.cpp")

add_library(webrtc_test_jni SHARED
    ${JNI_TEST_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/jni_bridge.cpp")

# The gtest-android-override dir must come BEFORE the real gtest include so
# our wrapper gtest/gtest.h is found first (it disables EXPECT_DEATH /
# ASSERT_DEATH which use fork() and crash the JVM in JNI context).
target_include_directories(webrtc_test_jni BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/gtest-android-override")

# The SDK uses include_directories() (directory-scoped), which does not
# propagate to targets defined outside its add_subdirectory scope.
target_include_directories(webrtc_test_jni PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/include"
    "${OPEN_SRC_INSTALL_PREFIX}/include")

# Resolve GTest target name (created by the SDK's build_dependency(gtest))
if(TARGET GTest::GTest)
    set(_GTEST GTest::GTest)
elseif(TARGET GTest::gtest)
    set(_GTEST GTest::gtest)
else()
    set(_GTEST gtest)
endif()

target_link_libraries(webrtc_test_jni PRIVATE
    kvsWebrtcClient
    ${OPENSSL_CRYPTO_LIBRARY}
    kvspicUtils
    ${_GTEST}
    log)

if(TARGET openssl_install)
    add_dependencies(webrtc_test_jni openssl_install)
endif()
