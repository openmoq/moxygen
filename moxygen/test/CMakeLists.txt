# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

if(NOT BUILD_TESTS)
    return()
endif()

# GTest/GMock is set up by cmake/MoxygenTest.cmake
# - Folly mode: uses getdeps GMock/GTest modules
# - Std mode: fetches googletest via FetchContent

if(MOXYGEN_USE_FOLLY)
    # =============================================================================
    # Folly-mode tests (full test suite with coroutines)
    # =============================================================================

    add_library(testmain TestMain.cpp)
    target_include_directories(
        testmain PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        ${LIBGMOCK_INCLUDE_DIR}
        ${LIBGTEST_INCLUDE_DIR}
    )
    target_link_libraries(testmain PUBLIC Folly::folly)
    target_compile_options(
        testmain PRIVATE
        ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
    )

    add_library(moqtestutils TestUtils.cpp)
    target_include_directories(
        moqtestutils PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        ${LIBGMOCK_INCLUDE_DIR}
        ${LIBGTEST_INCLUDE_DIR}
    )
    target_compile_options(
        moqtestutils PRIVATE
        ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
    )
    target_link_libraries(moqtestutils PUBLIC moxygen)

    add_library(moqsessiontestcommon MoQSessionTestCommon.cpp)
    target_include_directories(
        moqsessiontestcommon PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        ${LIBGMOCK_INCLUDE_DIR}
        ${LIBGTEST_INCLUDE_DIR}
    )
    target_compile_options(
        moqsessiontestcommon PRIVATE
        ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
    )
    target_link_libraries(
        moqsessiontestcommon PUBLIC
        moqtestutils
        moqrelaysession
        moqfollyexecutorimpl
        moxygenclient
        Folly::folly
    )

    moxygen_add_test(TARGET MoQTests
      PREFIX "MoQ"
      SOURCES
        MoQFramerTest.cpp
        MoQCodecTest.cpp
      DEPENDS
        moqtestutils
        testmain
    )

    moxygen_add_test(TARGET MoQSessionTests
      PREFIX "MoQSession"
      SOURCES
        MoQSessionFetchTests.cpp
        MoQSessionObjectDeliveryTests.cpp
        MoQSessionPublishNamespaceTests.cpp
        MoQSessionPublishTests.cpp
        MoQSessionSubscribeNamespaceTests.cpp
        MoQSessionSubscribeTests.cpp
        MoQSessionTests.cpp
        MoQSessionTrackStatusTests.cpp
      DEPENDS
        moqsessiontestcommon
        testmain
    )

    moxygen_add_test(TARGET MoQDeliveryTimerTests
      PREFIX "MoQDeliveryTimer"
      SOURCES
        MoQDeliveryTimerTest.cpp
      DEPENDS
        moqtestutils
        moqfollyexecutorimpl
        testmain
    )

else()
    # =============================================================================
    # Std-mode tests (no Folly dependencies)
    # =============================================================================

    # Enable GoogleTest discovery
    include(GoogleTest)

    # Framer tests for std-mode
    add_executable(MoQFramerTestStd MoQFramerTestStd.cpp)
    target_include_directories(
        MoQFramerTestStd PUBLIC
        $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
    )
    target_compile_options(
        MoQFramerTestStd PRIVATE
        ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
    )
    target_link_libraries(
        MoQFramerTestStd
        moxygen
        moqcompat
        GTest::gtest
        GTest::gmock
        Threads::Threads
    )
    gtest_discover_tests(MoQFramerTestStd)

    # Codec tests for std-mode
    add_executable(MoQCodecTestStd MoQCodecTestStd.cpp)
    target_include_directories(
        MoQCodecTestStd PUBLIC
        $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
    )
    target_compile_options(
        MoQCodecTestStd PRIVATE
        ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
    )
    target_link_libraries(
        MoQCodecTestStd
        moxygen
        moqcompat
        GTest::gtest
        GTest::gmock
        Threads::Threads
    )
    gtest_discover_tests(MoQCodecTestStd)

    # Test framework tests for std-mode
    add_executable(MoQTestFrameworkTest MoQTestFrameworkTest.cpp)
    target_include_directories(
        MoQTestFrameworkTest PUBLIC
        $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
    )
    target_compile_options(
        MoQTestFrameworkTest PRIVATE
        ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
    )
    target_link_libraries(
        MoQTestFrameworkTest
        moxygen
        moqcompat
        moqsimpleexecutor
        GTest::gtest
        GTest::gmock
        Threads::Threads
    )
    gtest_discover_tests(MoQTestFrameworkTest)

    # Session tests for std-mode (callback-based)
    add_executable(MoQSessionTestCompat MoQSessionTestCompat.cpp)
    target_include_directories(
        MoQSessionTestCompat PUBLIC
        $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
    )
    target_compile_options(
        MoQSessionTestCompat PRIVATE
        ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
    )
    target_link_libraries(
        MoQSessionTestCompat
        moxygen
        moqcompat
        moqsimpleexecutor
        GTest::gtest
        GTest::gmock
        Threads::Threads
    )
    gtest_discover_tests(MoQSessionTestCompat)

    message(STATUS "Building std-mode tests (MoQFramerTestStd, MoQCodecTestStd, MoQTestFrameworkTest, MoQSessionTestCompat)")

    # Add integration tests subdirectory (only for picoquic backend)
    add_subdirectory(integration)

endif() # MOXYGEN_USE_FOLLY
