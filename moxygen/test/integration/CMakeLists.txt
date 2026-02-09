# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# Integration tests for picoquic transport
# These tests require real network connections and TLS certificates.

if(NOT BUILD_TESTS)
    return()
endif()

# Integration tests are only available with picoquic backend
if(NOT MOXYGEN_QUIC_BACKEND STREQUAL "picoquic")
    message(STATUS "Skipping integration tests (picoquic backend not enabled)")
    return()
endif()

# Enable GoogleTest discovery
include(GoogleTest)

# Integration test library
add_library(moqintegrationtestbase STATIC
    IntegrationTestBase.cpp
)
target_include_directories(
    moqintegrationtestbase PUBLIC
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
    moqintegrationtestbase PRIVATE
    ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)
target_link_libraries(
    moqintegrationtestbase PUBLIC
    moxygen
    moqcompat
    moqsimpleexecutor
    moqtransport_picoquic
    GTest::gtest
    Threads::Threads
)

# Picoquic integration tests
add_executable(PicoquicIntegrationTest
    PicoquicIntegrationTest.cpp
)
target_include_directories(
    PicoquicIntegrationTest PUBLIC
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
    PicoquicIntegrationTest PRIVATE
    ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)
target_link_libraries(
    PicoquicIntegrationTest
    moqintegrationtestbase
    GTest::gtest
    GTest::gmock
    Threads::Threads
)

# Pass picoquic certs directory to tests
if(picoquic_SOURCE_DIR)
    target_compile_definitions(PicoquicIntegrationTest PRIVATE
        PICOQUIC_CERTS_DIR="${picoquic_SOURCE_DIR}/certs"
    )
endif()

gtest_discover_tests(PicoquicIntegrationTest
    PROPERTIES
        # Integration tests may be slower due to network operations
        TIMEOUT 60
)

message(STATUS "Building integration tests (PicoquicIntegrationTest)")
