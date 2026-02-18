# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# Folly + mvfst text client
if(MOXYGEN_USE_FOLLY AND MOXYGEN_QUIC_BACKEND STREQUAL "mvfst")

add_executable(
  moqtextclient
  MoQTextClient.cpp
)
set_target_properties(
  moqtextclient
  PROPERTIES
    BUILD_RPATH ${DEPS_LIBRARIES_DIR}
    INSTALL_RPATH ${DEPS_LIBRARIES_DIR}
)
target_include_directories(
  moqtextclient PUBLIC $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
  moqtextclient PRIVATE
  ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)
target_link_libraries(
  moqtextclient PUBLIC
  moxygenwtclient
  moqrelaysession
)

install(
    TARGETS moqtextclient
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)

endif() # MOXYGEN_USE_FOLLY AND mvfst

# Picoquic text client (works with both std-mode and Folly + picoquic)
if(MOXYGEN_QUIC_BACKEND STREQUAL "picoquic")

add_executable(
  picotextclient
  PicoTextClient.cpp
)
set_target_properties(
  picotextclient
  PROPERTIES
    BUILD_RPATH ${DEPS_LIBRARIES_DIR}
    INSTALL_RPATH ${DEPS_LIBRARIES_DIR}
)
target_include_directories(
  picotextclient PUBLIC $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
  picotextclient PRIVATE
  ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)
target_link_libraries(
  picotextclient PUBLIC
  moxygen
  moqcompat
  moqtransport_picoquic
  moqsimpleexecutor
  Threads::Threads
)
if(MOXYGEN_USE_FOLLY)
  target_link_libraries(picotextclient PUBLIC Folly::folly)
endif()

install(
    TARGETS picotextclient
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)

endif() # picoquic
