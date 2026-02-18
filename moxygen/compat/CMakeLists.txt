# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

if(MOXYGEN_USE_FOLLY)
    # Folly mode: header-only compat layer wrapping Folly types
    add_library(moqcompat INTERFACE)

    target_include_directories(
        moqcompat INTERFACE
        $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
    )

    target_compile_definitions(moqcompat INTERFACE MOXYGEN_USE_FOLLY=1)
    target_link_libraries(moqcompat INTERFACE Folly::folly)
else()
    # Std mode: header-only compat layer with Abseil containers
    add_library(moqcompat INTERFACE)

    target_include_directories(
        moqcompat INTERFACE
        $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
    )

    target_compile_definitions(moqcompat INTERFACE MOXYGEN_USE_FOLLY=0)

    # spdlog for logging
    target_link_libraries(moqcompat INTERFACE spdlog::spdlog)

    # Abseil for high-performance containers (Swiss table)
    find_package(absl REQUIRED)
    target_link_libraries(moqcompat INTERFACE
        absl::flat_hash_map
        absl::flat_hash_set
        absl::node_hash_map
        absl::node_hash_set
    )
endif()

install(
    TARGETS moqcompat
    EXPORT moxygen-exports
)
