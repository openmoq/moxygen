/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>
#include <moxygen/compat/Expected.h>
#include <moxygen/compat/Varint.h>

#if MOXYGEN_USE_FOLLY
#include <folly/io/Cursor.h>
#include <folly/io/IOBufQueue.h>
#else
#include <moxygen/compat/ByteBuffer.h>
#include <moxygen/compat/ByteBufferQueue.h>
#include <moxygen/compat/ByteCursor.h>
#endif

namespace moxygen::compat {

#if MOXYGEN_USE_FOLLY

// Folly mode: use native types
using Cursor = folly::io::Cursor;
using BufQueue = folly::IOBufQueue;
using ByteBufferQueue = folly::IOBufQueue;  // Alias for compat code
using WriteResult = folly::Expected<size_t, quic::TransportErrorCode>;

#else // !MOXYGEN_USE_FOLLY

// Std-mode: use our implementations
using Cursor = ByteCursor;
using BufQueue = ByteBufferQueue;
using WriteResult = Expected<size_t, quic::TransportErrorCode>;

#endif // MOXYGEN_USE_FOLLY

} // namespace moxygen::compat
