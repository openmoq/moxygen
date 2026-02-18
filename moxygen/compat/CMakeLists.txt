# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

if(MOXYGEN_USE_FOLLY)
    # Folly mode: header-only compat layer
    add_library(moqcompat INTERFACE)

    target_include_directories(
        moqcompat INTERFACE
        $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
    )

    target_compile_definitions(moqcompat INTERFACE MOXYGEN_USE_FOLLY=1)
    target_link_libraries(moqcompat INTERFACE Folly::folly)
else()
    # Std mode: compiled library with priority queue implementation
    add_library(moqcompat STATIC
        MoQPriorityQueue.cpp
    )

    target_include_directories(
        moqcompat PUBLIC
        $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
    )

    target_compile_definitions(moqcompat PUBLIC MOXYGEN_USE_FOLLY=0)

    # spdlog for logging (fetched in root CMakeLists.txt)
    target_link_libraries(moqcompat PUBLIC spdlog::spdlog)

    # Optional abseil support for high-performance containers
    # When enabled, FastMap/FastSet use absl::flat_hash_map/set (Swiss table)
    if(MOXYGEN_USE_ABSEIL)
        find_package(absl REQUIRED)
        target_compile_definitions(moqcompat PUBLIC MOXYGEN_USE_ABSEIL=1)
        target_link_libraries(moqcompat PUBLIC
            absl::flat_hash_map
            absl::flat_hash_set
            absl::node_hash_map
            absl::node_hash_set
        )
        message(STATUS "Using abseil containers (Swiss table)")
    else()
        target_compile_definitions(moqcompat PUBLIC MOXYGEN_USE_ABSEIL=0)
        message(STATUS "Using built-in RobinHood containers")
    endif()
endif()

install(
    TARGETS moqcompat
    EXPORT moxygen-exports
)
