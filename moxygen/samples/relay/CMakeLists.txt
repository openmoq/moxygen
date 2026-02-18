# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# Picoquic Relay Server
add_executable(picorelayserver PicoRelayServer.cpp)
target_include_directories(
  picorelayserver PUBLIC
  $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
  picorelayserver PRIVATE
  ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)
target_link_libraries(
  picorelayserver PUBLIC
  moxygen
  moqtransport_picoquic
  moqrelay_compat
  moqsimpleexecutor
)
if(MOXYGEN_USE_FOLLY)
  target_link_libraries(picorelayserver PUBLIC Folly::folly)
endif()

install(
    TARGETS picorelayserver
    EXPORT moxygen-exports
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
