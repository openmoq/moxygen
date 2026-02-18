/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY
#include <folly/logging/xlog.h>

#else

// =============================================================================
// Std-mode Debug Utilities
// =============================================================================
//
// Provides:
// - CHECK/DCHECK macros for assertions
// - XCHECK macros (folly-compatible CHECK variant)
// - XLOG macros via Logging.h (spdlog-backed)
//
// =============================================================================

#include <moxygen/compat/Logging.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <sstream>

// =============================================================================
// CHECK Macros - Always-on assertions
// =============================================================================

namespace moxygen::compat::detail {

[[noreturn]] inline void checkFailed(
    const char* file,
    int line,
    const char* expr,
    const std::string& msg = "") {
  std::cerr << file << ":" << line << ": CHECK failed: " << expr;
  if (!msg.empty()) {
    std::cerr << " (" << msg << ")";
  }
  std::cerr << std::endl;
  std::abort();
}

class CheckMessageVoidify {
 public:
  void operator&(std::ostream&) {}
};

class CheckMessage {
 public:
  CheckMessage(const char* file, int line, const char* expr)
      : file_(file), line_(line), expr_(expr) {}

  ~CheckMessage() {
    checkFailed(file_, line_, expr_, stream_.str());
  }

  std::ostream& stream() {
    return stream_;
  }

 private:
  const char* file_;
  int line_;
  const char* expr_;
  std::ostringstream stream_;
};

} // namespace moxygen::compat::detail

// CHECK with operator support
#define MOXYGEN_CHECK_OP(name, op, val1, val2)                          \
  if (!((val1)op(val2)))                                                \
  ::moxygen::compat::detail::CheckMessage(__FILE__, __LINE__, #val1 " " #op " " #val2).stream()

#define CHECK(condition)                                                 \
  if (!(condition))                                                      \
  ::moxygen::compat::detail::CheckMessage(__FILE__, __LINE__, #condition).stream()

#define CHECK_EQ(val1, val2) MOXYGEN_CHECK_OP(_EQ, ==, val1, val2)
#define CHECK_NE(val1, val2) MOXYGEN_CHECK_OP(_NE, !=, val1, val2)
#define CHECK_LE(val1, val2) MOXYGEN_CHECK_OP(_LE, <=, val1, val2)
#define CHECK_LT(val1, val2) MOXYGEN_CHECK_OP(_LT, <, val1, val2)
#define CHECK_GE(val1, val2) MOXYGEN_CHECK_OP(_GE, >=, val1, val2)
#define CHECK_GT(val1, val2) MOXYGEN_CHECK_OP(_GT, >, val1, val2)

// =============================================================================
// DCHECK Macros - Debug-only assertions (disabled in release builds)
// =============================================================================

#ifdef NDEBUG
#define DCHECK(condition) \
  while (false) ::moxygen::compat::detail::CheckMessageVoidify() & std::cerr
#define DCHECK_EQ(val1, val2) \
  while (false) ::moxygen::compat::detail::CheckMessageVoidify() & std::cerr
#define DCHECK_NE(val1, val2) \
  while (false) ::moxygen::compat::detail::CheckMessageVoidify() & std::cerr
#define DCHECK_LE(val1, val2) \
  while (false) ::moxygen::compat::detail::CheckMessageVoidify() & std::cerr
#define DCHECK_LT(val1, val2) \
  while (false) ::moxygen::compat::detail::CheckMessageVoidify() & std::cerr
#define DCHECK_GE(val1, val2) \
  while (false) ::moxygen::compat::detail::CheckMessageVoidify() & std::cerr
#define DCHECK_GT(val1, val2) \
  while (false) ::moxygen::compat::detail::CheckMessageVoidify() & std::cerr
#else
#define DCHECK(condition) CHECK(condition)
#define DCHECK_EQ(val1, val2) CHECK_EQ(val1, val2)
#define DCHECK_NE(val1, val2) CHECK_NE(val1, val2)
#define DCHECK_LE(val1, val2) CHECK_LE(val1, val2)
#define DCHECK_LT(val1, val2) CHECK_LT(val1, val2)
#define DCHECK_GE(val1, val2) CHECK_GE(val1, val2)
#define DCHECK_GT(val1, val2) CHECK_GT(val1, val2)
#endif

// =============================================================================
// XCHECK Macros - Folly-compatible CHECK variant
// =============================================================================

#define XCHECK(condition) CHECK(condition)
#define XCHECK_EQ(val1, val2) CHECK_EQ(val1, val2)
#define XCHECK_NE(val1, val2) CHECK_NE(val1, val2)
#define XCHECK_LE(val1, val2) CHECK_LE(val1, val2)
#define XCHECK_LT(val1, val2) CHECK_LT(val1, val2)
#define XCHECK_GE(val1, val2) CHECK_GE(val1, val2)
#define XCHECK_GT(val1, val2) CHECK_GT(val1, val2)

// =============================================================================
// Legacy LOG Macros (for compatibility)
// =============================================================================

// Simple LOG macros that map to XLOG
#define LOG(severity) XLOG(severity)
#define VLOG(level) if (false) std::cerr

// =============================================================================
// Miscellaneous
// =============================================================================

// FMT_FALLTHROUGH - explicit fallthrough in switch statements
#if defined(__has_cpp_attribute) && __has_cpp_attribute(fallthrough)
#define FMT_FALLTHROUGH [[fallthrough]]
#elif defined(__GNUC__) && __GNUC__ >= 7
#define FMT_FALLTHROUGH __attribute__((fallthrough))
#else
#define FMT_FALLTHROUGH ((void)0)
#endif

#endif // MOXYGEN_USE_FOLLY
