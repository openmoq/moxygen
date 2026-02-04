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

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <sstream>

// Std-mode: Simple CHECK macros using assert
// In release builds, these become no-ops unless NDEBUG is not defined

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

// DCHECK variants - only active in debug builds
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

// LOG macros - simple stdout/stderr output
#define LOG(severity) std::cerr << "[" #severity "] "
#define VLOG(level) if (false) std::cerr

// XLOG macros - compatible with folly's XLOG
// In std-mode, these are simple wrappers around LOG or no-ops
#define XLOG(level) std::cerr << "[" #level "] "
#define XLOG_IF(level, condition) if (condition) XLOG(level)

// Debug level XLOG - disabled by default in std-mode
// DBG0 is highest priority, DBG9 is lowest
#ifndef MOXYGEN_DEBUG_LEVEL
#define MOXYGEN_DEBUG_LEVEL 0  // Only show critical debug messages by default
#endif

// XLOG with debug levels - only emit if level is <= MOXYGEN_DEBUG_LEVEL
#define XLOG_DBG_IMPL(level, num)           \
  if (num <= MOXYGEN_DEBUG_LEVEL)           \
  std::cerr << "[DBG" #num "] "

// Specific DBG levels - most are disabled in release
#define XLOG_DBG0() XLOG_DBG_IMPL(DBG0, 0)
#define XLOG_DBG1() if (false) std::cerr
#define XLOG_DBG2() if (false) std::cerr
#define XLOG_DBG3() if (false) std::cerr
#define XLOG_DBG4() if (false) std::cerr
#define XLOG_DBG5() if (false) std::cerr
#define XLOG_DBG6() if (false) std::cerr
#define XLOG_DBG7() if (false) std::cerr
#define XLOG_DBG8() if (false) std::cerr
#define XLOG_DBG9() if (false) std::cerr

// Macro to handle XLOG(DBGn) syntax
// This is a workaround since we can't easily overload on the parameter
#define DBG0 DBG0
#define DBG1 DBG1
#define DBG2 DBG2
#define DBG3 DBG3
#define DBG4 DBG4
#define DBG5 DBG5
#define DBG6 DBG6
#define DBG7 DBG7
#define DBG8 DBG8
#define DBG9 DBG9
#define INFO INFO
#define WARN WARN
#define ERR ERR
#define FATAL FATAL

// Redefine XLOG to handle the different log levels
#undef XLOG
#define XLOG_LEVEL_DBG if (false) std::cerr
#define XLOG_LEVEL_DBG0 if (false) std::cerr
#define XLOG_LEVEL_DBG1 if (false) std::cerr
#define XLOG_LEVEL_DBG2 if (false) std::cerr
#define XLOG_LEVEL_DBG3 if (false) std::cerr
#define XLOG_LEVEL_DBG4 if (false) std::cerr
#define XLOG_LEVEL_DBG5 if (false) std::cerr
#define XLOG_LEVEL_DBG6 if (false) std::cerr
#define XLOG_LEVEL_DBG7 if (false) std::cerr
#define XLOG_LEVEL_DBG8 if (false) std::cerr
#define XLOG_LEVEL_DBG9 if (false) std::cerr
#define XLOG_LEVEL_INFO std::cerr << "[INFO] "
#define XLOG_LEVEL_WARN std::cerr << "[WARN] "
#define XLOG_LEVEL_WARNING std::cerr << "[WARN] "
#define XLOG_LEVEL_ERR std::cerr << "[ERR] "
#define XLOG_LEVEL_ERROR std::cerr << "[ERR] "
#define XLOG_LEVEL_FATAL std::cerr << "[FATAL] "

#define XLOG_CONCAT(a, b) a##b
#define XLOG(level) XLOG_CONCAT(XLOG_LEVEL_, level)

#endif // MOXYGEN_USE_FOLLY
