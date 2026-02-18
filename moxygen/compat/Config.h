/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// MOXYGEN_USE_FOLLY controls whether to use Folly types or std alternatives.
// When ON (default): Use Folly IOBuf, Expected, F14 containers, coroutines
// When OFF: Use std-mode alternatives (ByteBuffer, std containers, callbacks)
//
// This is set by CMake. Default to 1 (use Folly) if not explicitly set.
#ifndef MOXYGEN_USE_FOLLY
#define MOXYGEN_USE_FOLLY 1
#endif
