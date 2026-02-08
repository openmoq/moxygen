# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# Folly + mvfst date server
if(MOXYGEN_USE_FOLLY AND MOXYGEN_QUIC_BACKEND STREQUAL "mvfst")

add_executable(
  moqdateserver
  MoQDateServer.cpp
)
set_target_properties(
  moqdateserver
  PROPERTIES
    BUILD_RPATH ${DEPS_LIBRARIES_DIR}
    INSTALL_RPATH ${DEPS_LIBRARIES_DIR}
)
target_include_directories(
  moqdateserver PUBLIC $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
  moqdateserver PRIVATE
  ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)
target_link_libraries(
  moqdateserver PUBLIC
  moqrelay
  moxygenserver
  moxygenwtclient
)

install(
    TARGETS moqdateserver
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)

endif() # MOXYGEN_USE_FOLLY AND mvfst

# Picoquic date server (std-mode only for now)
# TODO: Add Folly + picoquic support
if(MOXYGEN_QUIC_BACKEND STREQUAL "picoquic" AND NOT MOXYGEN_USE_FOLLY)

add_executable(
  picodateserver
  PicoDateServer.cpp
)
set_target_properties(
  picodateserver
  PROPERTIES
    BUILD_RPATH ${DEPS_LIBRARIES_DIR}
    INSTALL_RPATH ${DEPS_LIBRARIES_DIR}
)
target_include_directories(
  picodateserver PUBLIC $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
  picodateserver PRIVATE
  ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)
target_link_libraries(
  picodateserver PUBLIC
  moxygen
  moqcompat
  moqtransport_picoquic
  moqsimpleexecutor
  Threads::Threads
)
if(MOXYGEN_USE_FOLLY)
  target_link_libraries(picodateserver PUBLIC Folly::folly)
endif()

install(
    TARGETS picodateserver
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)

endif() # picoquic
