# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# Header-only interface library for compat types
add_library(moqcompat INTERFACE)

target_include_directories(
    moqcompat INTERFACE
    $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)

if(MOXYGEN_USE_FOLLY)
    target_compile_definitions(moqcompat INTERFACE MOXYGEN_USE_FOLLY=1)
    target_link_libraries(moqcompat INTERFACE Folly::folly)
else()
    target_compile_definitions(moqcompat INTERFACE MOXYGEN_USE_FOLLY=0)
endif()
