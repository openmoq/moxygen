# Copyright (c) OpenMOQ contributors.
# This source code is licensed under the Apache 2.0 license found in the
# LICENSE file in the root directory of this source tree.

moxygen_add_library(openmoq_extensions
  SRCS
    MoQClusterExtension.cpp
    TopTracksExtension.cpp
  EXPORTED_DEPS
    moxygen_moq
    moxygen_moq_framer
    moxygen_moq_types
    moxygen_moq_varint
    mvfst::mvfst_folly_utils
    Folly::folly_container_f14_hash
    Folly::folly_expected
    Folly::folly_io_iobuf
)

add_subdirectory(test)
