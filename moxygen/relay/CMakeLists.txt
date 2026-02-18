# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# Relay components (shared between Folly and std-mode)
# MoQForwarder can now be built in std-mode with callback-based APIs

# MoQCache - requires Folly + mvfst (coroutine-based APIs)
# Not compatible with picoquic backend which uses callback-based APIs
if(MOXYGEN_USE_FOLLY AND MOXYGEN_QUIC_BACKEND STREQUAL "mvfst")
add_library(moqcache MoQCache.cpp)
target_include_directories(
  moqcache PUBLIC
  $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
  moqcache PRIVATE
  ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)
target_link_libraries(
  moqcache PUBLIC
  moxygen
  Folly::folly
)

install(
    TARGETS moqcache
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)
endif() # MOXYGEN_USE_FOLLY AND mvfst

# Folly + mvfst: Full relay implementation with coroutines
if(MOXYGEN_USE_FOLLY AND MOXYGEN_QUIC_BACKEND STREQUAL "mvfst")

add_library(moqrelay MoQRelay.cpp MoQCache.cpp MoQForwarder.cpp)
target_include_directories(
  moqrelay PUBLIC
  $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
  moqrelay PRIVATE
  ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)
target_link_libraries(
  moqrelay PUBLIC
  Folly::folly
  proxygen::proxygen
  proxygen::proxygenhqserver
  moxygen
  moqrelaysession
)

install(
    TARGETS moqrelay
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)

# MoQRelayServer
add_executable(
  moqrelayserver
  MoQRelayServer.cpp
)
set_target_properties(
  moqrelayserver
  PROPERTIES
    BUILD_RPATH ${DEPS_LIBRARIES_DIR}
    INSTALL_RPATH ${DEPS_LIBRARIES_DIR}
)
target_include_directories(
  moqrelayserver PUBLIC
  $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
  moqrelayserver PRIVATE
  ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)
target_link_libraries(
  moqrelayserver PUBLIC
  moqrelay
  moxygenserver
)

install(
    TARGETS moqrelayserver
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)

add_subdirectory(test)

endif() # MOXYGEN_USE_FOLLY AND mvfst

# Picoquic: Relay libraries (callback-based, no coroutines)
if(MOXYGEN_QUIC_BACKEND STREQUAL "picoquic")

add_library(moqforwarder_compat MoQForwarder.cpp)
target_include_directories(
  moqforwarder_compat PUBLIC
  $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
  moqforwarder_compat PRIVATE
  ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)
target_link_libraries(
  moqforwarder_compat PUBLIC
  moxygen
  moqcompat
)
if(MOXYGEN_USE_FOLLY)
  target_link_libraries(moqforwarder_compat PUBLIC Folly::folly)
endif()

install(
    TARGETS moqforwarder_compat
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)

# MoQRelayCompat - simplified relay for picoquic/std-mode
add_library(moqrelay_compat MoQRelayCompat.cpp)
target_include_directories(
  moqrelay_compat PUBLIC
  $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
  moqrelay_compat PRIVATE
  ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)
target_link_libraries(
  moqrelay_compat PUBLIC
  moxygen
  moqcompat
  moqforwarder_compat
)
if(MOXYGEN_USE_FOLLY)
  target_link_libraries(moqrelay_compat PUBLIC Folly::folly)
endif()

install(
    TARGETS moqrelay_compat
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)

endif() # picoquic
