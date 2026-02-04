# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# Transport implementations for different QUIC backends

# --- mvfst/proxygen WebTransport adapter ---
if(MOXYGEN_QUIC_BACKEND STREQUAL "mvfst")
  add_library(moqtransport_proxygen
    ProxygenWebTransportAdapter.cpp
  )

  target_include_directories(
    moqtransport_proxygen PUBLIC
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
  )

  target_compile_options(
    moqtransport_proxygen PRIVATE
    ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
  )

  target_link_libraries(
    moqtransport_proxygen PUBLIC
    moqcompat
    Folly::folly
    proxygen::proxygen
  )

  install(
    TARGETS moqtransport_proxygen
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
  )

  # MvfstStdModeAdapter - allows std-mode code to use mvfst transport
  # This adapter wraps mvfst/proxygen and provides a callback-based interface
  add_library(moqtransport_mvfst_stdmode
    MvfstStdModeAdapter.cpp
  )

  target_include_directories(
    moqtransport_mvfst_stdmode PUBLIC
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
  )

  target_compile_options(
    moqtransport_mvfst_stdmode PRIVATE
    ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
  )

  target_link_libraries(
    moqtransport_mvfst_stdmode PUBLIC
    moxygen
    moqcompat
    Folly::folly
    proxygen::proxygen
    proxygen::quicwebtransport
  )

  install(
    TARGETS moqtransport_mvfst_stdmode
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
  )
endif()

# --- picoquic transport implementation ---
if(MOXYGEN_QUIC_BACKEND STREQUAL "picoquic")
  add_library(moqtransport_picoquic
    PicoquicWebTransport.cpp
    PicoquicMoQClient.cpp
    PicoquicMoQServer.cpp
  )

  target_include_directories(
    moqtransport_picoquic PUBLIC
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
    ${picoquic_SOURCE_DIR}/picoquic
    ${picoquic_SOURCE_DIR}/picohttp
    ${picotls_SOURCE_DIR}/include
  )

  target_compile_options(
    moqtransport_picoquic PRIVATE
    ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
  )

  target_link_libraries(
    moqtransport_picoquic PUBLIC
    moqcompat
    picoquic-core
    picohttp-core
    Threads::Threads
  )

  # Add Folly if available
  if(MOXYGEN_USE_FOLLY)
    target_link_libraries(
      moqtransport_picoquic PUBLIC
      Folly::folly
    )
  endif()

  # Note: We don't export moqtransport_picoquic because picoquic-core/picohttp-core
  # are fetched dependencies and not in our export set. Users who want to use
  # picoquic will need to build moxygen from source.
  install(
    TARGETS moqtransport_picoquic
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
  )
endif()
