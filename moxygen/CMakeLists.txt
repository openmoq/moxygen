# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# Compat layer (must be first)
add_subdirectory(compat)

# MOQ Core Library
# Note: MoQSession (coroutine-based) requires Folly + mvfst/proxygen.
# MoQSessionCompat (callback-based) is used for std-mode or Folly + picoquic.
if(MOXYGEN_USE_FOLLY AND MOXYGEN_QUIC_BACKEND STREQUAL "mvfst")
    # Folly + mvfst: full coroutine-based session
    set(MOXYGEN_CORE_SOURCES
        MoQFramer.cpp
        MoQCodec.cpp
        MoQSession.cpp
        MoQTokenCache.cpp
        MoQTypes.cpp
        MoQVersions.cpp
        events/MoQDeliveryTimer.cpp
    )
elseif(MOXYGEN_USE_FOLLY)
    # Folly + picoquic: use callback-based session with Folly coroutine support
    set(MOXYGEN_CORE_SOURCES
        MoQFramer.cpp
        MoQCodec.cpp
        MoQSessionCompat.cpp
        MoQRelaySessionCompat.cpp
        MoQTokenCache.cpp
        MoQTypes.cpp
        MoQVersions.cpp
        events/MoQDeliveryTimer.cpp
    )
else()
    # Std-mode: callback-based session, no Folly dependencies
    set(MOXYGEN_CORE_SOURCES
        MoQFramer.cpp
        MoQCodec.cpp
        MoQSessionCompat.cpp
        MoQRelaySessionCompat.cpp
        MoQTokenCache.cpp
        MoQTypes.cpp
        MoQVersions.cpp
        events/MoQDeliveryTimer.cpp
    )
endif()

add_library(moxygen ${MOXYGEN_CORE_SOURCES})

target_include_directories(
    moxygen PUBLIC
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
    moxygen PRIVATE
    ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)

if(MOXYGEN_USE_FOLLY)
    # Build mlog first so mlogger is available for moxygen to link
    add_subdirectory(mlog)

    target_link_libraries(
        moxygen PUBLIC
        moqcompat
        mlogger
        Folly::folly
        wangle::wangle
    )
    # Add mvfst/proxygen deps only when using mvfst backend
    if(MOXYGEN_QUIC_BACKEND STREQUAL "mvfst")
        target_link_libraries(
            moxygen PUBLIC
            proxygen::proxygen
            mvfst::mvfst_events
        )
    endif()
else()
    # Std-mode: minimal dependencies
    target_link_libraries(
        moxygen PUBLIC
        moqcompat
        Threads::Threads
    )
endif()

install(
    TARGETS moxygen
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)

# Transport implementations
# mvfst/proxygen adapter requires Folly
if(MOXYGEN_USE_FOLLY AND MOXYGEN_QUIC_BACKEND STREQUAL "mvfst")
  add_subdirectory(transports)
endif()

# picoquic transport (works with or without Folly)
if(MOXYGEN_QUIC_BACKEND STREQUAL "picoquic")
  add_subdirectory(transports)
endif()

# Folly + mvfst targets (require proxygen/WebTransport)
if(MOXYGEN_USE_FOLLY AND MOXYGEN_QUIC_BACKEND STREQUAL "mvfst")

add_library(moqrelaysession
    MoQRelaySession.cpp
)

target_include_directories(
    moqrelaysession PUBLIC
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)

target_compile_options(
    moqrelaysession PRIVATE
    ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)

target_link_libraries(
    moqrelaysession PUBLIC
    moxygen
    Folly::folly
)

install(
    TARGETS moqrelaysession
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)

# mvfst-specific components (executor, server, client libraries)
if(MOXYGEN_QUIC_BACKEND STREQUAL "mvfst")

add_library(moqfollyexecutorimpl
    events/MoQFollyExecutorImpl.cpp
)

target_include_directories(
    moqfollyexecutorimpl PUBLIC
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)

target_compile_options(
    moqfollyexecutorimpl PRIVATE
    ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)

target_link_libraries(
    moqfollyexecutorimpl PUBLIC
    Folly::folly
    mvfst::mvfst_events
)

add_library(moxygenserver
    MoQServerBase.cpp
    MoQServer.cpp)

target_include_directories(
    moxygenserver PUBLIC
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)

target_compile_options(
    moxygenserver PRIVATE
    ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)

target_link_libraries(
    moxygenserver PUBLIC
    moxygen
    mlogger
    proxygen::proxygenhqserver
    proxygen::quicwebtransport
    moqfollyexecutorimpl
)

add_library(moxygenclient
    MoQClient.cpp
    MoQClientBase.cpp
    util/QuicConnector.cpp
)

target_include_directories(
    moxygenclient PUBLIC
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)

target_compile_options(
    moxygenclient PRIVATE
    ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)

target_link_libraries(
    moxygenclient PUBLIC
    moxygen
    mlogger
    proxygen::proxygenhqserver
    proxygen::quicwebtransport
    moqfollyexecutorimpl
)

add_library(moxygenwtclient
    MoQWebTransportClient.cpp
)

target_include_directories(
    moxygenwtclient PUBLIC
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)

target_compile_options(
    moxygenwtclient PRIVATE
    ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)

target_link_libraries(
    moxygenwtclient PUBLIC
    moxygenclient
    proxygen::proxygenhqserver
)

endif() # MOXYGEN_QUIC_BACKEND == mvfst

# relay and related components require mvfst/proxygen
if(MOXYGEN_QUIC_BACKEND STREQUAL "mvfst")
  add_subdirectory(relay)
  # TODO: Fails with compiler error in ubuntu P1361252588
  # add_subdirectory(samples/chat)
  add_subdirectory(samples/text-client)
  add_subdirectory(samples/date)
  add_subdirectory(samples/flv_streamer_client)
  add_subdirectory(samples/flv_receiver_client)
  add_subdirectory(moq_mi)
  add_subdirectory(moqtest)
  add_subdirectory(flv_parser)
  add_subdirectory(test)
endif()

endif() # MOXYGEN_USE_FOLLY AND MOXYGEN_QUIC_BACKEND == mvfst

# Picoquic-mode components (works with or without Folly)
if(MOXYGEN_QUIC_BACKEND STREQUAL "picoquic")

# Simple executor for std-mode (no Folly dependency)
add_library(moqsimpleexecutor
    events/MoQSimpleExecutor.cpp
)

target_include_directories(
    moqsimpleexecutor PUBLIC
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)

target_compile_options(
    moqsimpleexecutor PRIVATE
    ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)

target_link_libraries(
    moqsimpleexecutor PUBLIC
    moqcompat
    Threads::Threads
)

install(
    TARGETS moqsimpleexecutor
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)

# Picoquic sample programs
add_subdirectory(samples/date)
add_subdirectory(samples/text-client)

endif() # MOXYGEN_QUIC_BACKEND == picoquic
