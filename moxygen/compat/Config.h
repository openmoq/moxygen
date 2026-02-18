/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// MOXYGEN_USE_FOLLY controls whether to use folly types or std alternatives.
// Default to 1 (use folly) if not explicitly set by CMake.
#ifndef MOXYGEN_USE_FOLLY
#define MOXYGEN_USE_FOLLY 1
#endif

// MOXYGEN_USE_ABSEIL controls whether to use abseil containers in std mode.
// When enabled, FastMap/FastSet use absl::flat_hash_map/set (Swiss table).
// When disabled, they use built-in RobinHoodMap/Set.
// Default to 0 (use built-in) if not explicitly set by CMake.
// Only applies when MOXYGEN_USE_FOLLY is 0.
#ifndef MOXYGEN_USE_ABSEIL
#define MOXYGEN_USE_ABSEIL 0
#endif

// QUIC backend selection
// MOXYGEN_QUIC_MVFST - Use mvfst/proxygen QUIC stack
// MOXYGEN_QUIC_PICOQUIC - Use picoquic QUIC stack
// Default to mvfst if not explicitly set by CMake.
#ifndef MOXYGEN_QUIC_MVFST
#define MOXYGEN_QUIC_MVFST 1
#endif

#ifndef MOXYGEN_QUIC_PICOQUIC
#define MOXYGEN_QUIC_PICOQUIC 0
#endif

// Validate configuration - exactly one QUIC backend must be selected
#if MOXYGEN_QUIC_MVFST && MOXYGEN_QUIC_PICOQUIC
#error "Cannot enable both MOXYGEN_QUIC_MVFST and MOXYGEN_QUIC_PICOQUIC"
#endif

#if !MOXYGEN_QUIC_MVFST && !MOXYGEN_QUIC_PICOQUIC
#error "Must enable either MOXYGEN_QUIC_MVFST or MOXYGEN_QUIC_PICOQUIC"
#endif
