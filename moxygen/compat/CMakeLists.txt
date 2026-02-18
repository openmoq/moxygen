# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# Compat library - header-only now, but we keep a library target for linking
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

    # Optional abseil support for high-performance containers
    # When enabled, FastMap/FastSet use absl::flat_hash_map/set (Swiss table)
    if(MOXYGEN_USE_ABSEIL)
        find_package(absl REQUIRED)
        target_compile_definitions(moqcompat INTERFACE MOXYGEN_USE_ABSEIL=1)
        target_link_libraries(moqcompat INTERFACE
            absl::flat_hash_map
            absl::flat_hash_set
            absl::node_hash_map
            absl::node_hash_set
        )
        message(STATUS "Using abseil containers (Swiss table)")
    else()
        target_compile_definitions(moqcompat INTERFACE MOXYGEN_USE_ABSEIL=0)
        message(STATUS "Using built-in RobinHood containers")
    endif()
endif()

install(
    TARGETS moqcompat
    EXPORT moxygen-exports
)
