# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# Compat library with payload implementations
add_library(moqcompat
    PayloadImpl.cpp
)

if(MOXYGEN_USE_FOLLY)
    target_sources(moqcompat PRIVATE IOBufPayload.cpp)
endif()

target_include_directories(
    moqcompat PUBLIC
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)

target_compile_options(
    moqcompat PRIVATE
    ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)

if(MOXYGEN_USE_FOLLY)
    target_compile_definitions(moqcompat PUBLIC MOXYGEN_USE_FOLLY=1)
    target_link_libraries(moqcompat PUBLIC Folly::folly)
else()
    target_compile_definitions(moqcompat PUBLIC MOXYGEN_USE_FOLLY=0)
endif()

install(
    TARGETS moqcompat
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)
