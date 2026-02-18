/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY
// Folly mode: use folly's xlog directly
#include <folly/logging/xlog.h>

#else
// Std-mode Logging using spdlog
// Provides XLOG-compatible macros backed by spdlog.

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/fmt/ostr.h>

#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace moxygen::logging {

// =============================================================================
// Log Level Definitions
// =============================================================================

enum class LogLevel {
  DBG9 = 0,  // Most verbose debug
  DBG8 = 1,
  DBG7 = 2,
  DBG6 = 3,
  DBG5 = 4,
  DBG4 = 5,
  DBG3 = 6,
  DBG2 = 7,
  DBG1 = 8,
  DBG0 = 9,  // Least verbose debug
  DBG = 9,   // Alias for DBG0
  INFO = 10,
  WARN = 11,
  WARNING = 11,
  ERR = 12,
  ERROR = 12,
  CRITICAL = 13,
  FATAL = 14,
};

// Convert our log level to spdlog level
inline spdlog::level::level_enum toSpdlogLevel(LogLevel level) {
  switch (level) {
    case LogLevel::DBG9:
    case LogLevel::DBG8:
    case LogLevel::DBG7:
    case LogLevel::DBG6:
    case LogLevel::DBG5:
    case LogLevel::DBG4:
    case LogLevel::DBG3:
    case LogLevel::DBG2:
    case LogLevel::DBG1:
    case LogLevel::DBG0:
      return spdlog::level::debug;
    case LogLevel::INFO:
      return spdlog::level::info;
    case LogLevel::WARN:
      return spdlog::level::warn;
    case LogLevel::ERR:
      return spdlog::level::err;
    case LogLevel::CRITICAL:
    case LogLevel::FATAL:
      return spdlog::level::critical;
    default:
      return spdlog::level::info;
  }
}

// =============================================================================
// Logger Singleton
// =============================================================================

class Logger {
 public:
  static Logger& instance() {
    static Logger logger;
    return logger;
  }

  std::shared_ptr<spdlog::logger>& get() {
    return logger_;
  }

  // Set minimum log level
  void setLevel(LogLevel level) {
    minLevel_ = level;
    logger_->set_level(toSpdlogLevel(level));
  }

  // Set minimum log level for debug messages (DBG0-DBG9)
  void setDebugLevel(int level) {
    debugLevel_ = level;
    // If debug level is set, ensure spdlog shows debug messages
    if (level > 0) {
      logger_->set_level(spdlog::level::debug);
    }
  }

  LogLevel minLevel() const { return minLevel_; }
  int debugLevel() const { return debugLevel_; }

  // Check if a log level should be emitted
  bool shouldLog(LogLevel level) const {
    // For debug levels, check debugLevel_
    if (level <= LogLevel::DBG0) {
      int debugNum = static_cast<int>(LogLevel::DBG0) - static_cast<int>(level);
      return debugNum <= debugLevel_;
    }
    return level >= minLevel_;
  }

  // Flush all pending log messages
  void flush() {
    logger_->flush();
  }

 private:
  Logger() {
    // Create colored console logger
    logger_ = spdlog::stdout_color_mt("moxygen");

    // Default pattern: [timestamp] [level] [source] message
    logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");

    // Default level: INFO (debug disabled by default)
    logger_->set_level(spdlog::level::info);

    // Flush on error or higher
    logger_->flush_on(spdlog::level::err);
  }

  std::shared_ptr<spdlog::logger> logger_;
  LogLevel minLevel_{LogLevel::INFO};
  int debugLevel_{0};  // 0 = no debug, 9 = all debug levels
};

// =============================================================================
// Stream-style Log Message Builder
// =============================================================================

class LogMessage {
 public:
  LogMessage(LogLevel level, const char* file, int line)
      : level_(level), file_(file), line_(line) {}

  ~LogMessage() {
    if (Logger::instance().shouldLog(level_)) {
      auto& logger = Logger::instance().get();
      logger->log(
          spdlog::source_loc{file_, line_, ""},
          toSpdlogLevel(level_),
          "{}",
          stream_.str());

      // Abort on FATAL
      if (level_ == LogLevel::FATAL) {
        logger->flush();
        std::abort();
      }
    }
  }

  std::ostream& stream() { return stream_; }

 private:
  LogLevel level_;
  const char* file_;
  int line_;
  std::ostringstream stream_;
};

// Voidify helper for conditional logging
class LogMessageVoidify {
 public:
  void operator&(std::ostream&) {}
};

// =============================================================================
// XLOG Macros - Stream Style (compatible with folly::XLOG)
// =============================================================================

#define MOXYGEN_LOG_LEVEL_DBG9 ::moxygen::logging::LogLevel::DBG9
#define MOXYGEN_LOG_LEVEL_DBG8 ::moxygen::logging::LogLevel::DBG8
#define MOXYGEN_LOG_LEVEL_DBG7 ::moxygen::logging::LogLevel::DBG7
#define MOXYGEN_LOG_LEVEL_DBG6 ::moxygen::logging::LogLevel::DBG6
#define MOXYGEN_LOG_LEVEL_DBG5 ::moxygen::logging::LogLevel::DBG5
#define MOXYGEN_LOG_LEVEL_DBG4 ::moxygen::logging::LogLevel::DBG4
#define MOXYGEN_LOG_LEVEL_DBG3 ::moxygen::logging::LogLevel::DBG3
#define MOXYGEN_LOG_LEVEL_DBG2 ::moxygen::logging::LogLevel::DBG2
#define MOXYGEN_LOG_LEVEL_DBG1 ::moxygen::logging::LogLevel::DBG1
#define MOXYGEN_LOG_LEVEL_DBG0 ::moxygen::logging::LogLevel::DBG0
#define MOXYGEN_LOG_LEVEL_DBG  ::moxygen::logging::LogLevel::DBG
#define MOXYGEN_LOG_LEVEL_INFO ::moxygen::logging::LogLevel::INFO
#define MOXYGEN_LOG_LEVEL_WARN ::moxygen::logging::LogLevel::WARN
#define MOXYGEN_LOG_LEVEL_WARNING ::moxygen::logging::LogLevel::WARNING
#define MOXYGEN_LOG_LEVEL_ERR ::moxygen::logging::LogLevel::ERR
#define MOXYGEN_LOG_LEVEL_ERROR ::moxygen::logging::LogLevel::ERROR
#define MOXYGEN_LOG_LEVEL_CRITICAL ::moxygen::logging::LogLevel::CRITICAL
#define MOXYGEN_LOG_LEVEL_FATAL ::moxygen::logging::LogLevel::FATAL

#define MOXYGEN_LOG_CONCAT_IMPL(a, b) a##b
#define MOXYGEN_LOG_CONCAT(a, b) MOXYGEN_LOG_CONCAT_IMPL(a, b)

// Main XLOG macro - stream style
#define XLOG(level) \
  ::moxygen::logging::LogMessage( \
      MOXYGEN_LOG_CONCAT(MOXYGEN_LOG_LEVEL_, level), \
      __FILE__, \
      __LINE__).stream()

// Conditional XLOG
#define XLOG_IF(level, condition) \
  !(condition) ? (void)0 : \
  ::moxygen::logging::LogMessageVoidify() & XLOG(level)

// XLOG with explicit check (for hot paths)
#define XLOG_IS_ON(level) \
  ::moxygen::logging::Logger::instance().shouldLog( \
      MOXYGEN_LOG_CONCAT(MOXYGEN_LOG_LEVEL_, level))

// =============================================================================
// XLOGF Macros - Format String Style (like fmt::format)
// =============================================================================

// Helper for format-style logging
#define XLOGF_IMPL(level, ...) \
  do { \
    auto& logger = ::moxygen::logging::Logger::instance(); \
    if (logger.shouldLog(MOXYGEN_LOG_CONCAT(MOXYGEN_LOG_LEVEL_, level))) { \
      logger.get()->log( \
          spdlog::source_loc{__FILE__, __LINE__, ""}, \
          ::moxygen::logging::toSpdlogLevel( \
              MOXYGEN_LOG_CONCAT(MOXYGEN_LOG_LEVEL_, level)), \
          __VA_ARGS__); \
      if (MOXYGEN_LOG_CONCAT(MOXYGEN_LOG_LEVEL_, level) == \
          ::moxygen::logging::LogLevel::FATAL) { \
        logger.flush(); \
        std::abort(); \
      } \
    } \
  } while (false)

#define XLOGF(level, ...) XLOGF_IMPL(level, __VA_ARGS__)

// =============================================================================
// Convenience Functions
// =============================================================================

// Initialize logging (call once at startup)
inline void initLogging(LogLevel level = LogLevel::INFO, int debugLevel = 0) {
  auto& logger = Logger::instance();
  logger.setLevel(level);
  logger.setDebugLevel(debugLevel);
}

// Set log level at runtime
inline void setLogLevel(LogLevel level) {
  Logger::instance().setLevel(level);
}

// Set debug verbosity (0-9, where 9 shows all DBG levels)
inline void setDebugLevel(int level) {
  Logger::instance().setDebugLevel(level);
}

// Flush pending log messages
inline void flushLogs() {
  Logger::instance().flush();
}

} // namespace moxygen::logging

// Bring commonly used items into compat namespace
namespace moxygen::compat {
using logging::LogLevel;
using logging::initLogging;
using logging::setLogLevel;
using logging::setDebugLevel;
using logging::flushLogs;
} // namespace moxygen::compat

// Define level names for XLOG(LEVEL) syntax
#define DBG9 DBG9
#define DBG8 DBG8
#define DBG7 DBG7
#define DBG6 DBG6
#define DBG5 DBG5
#define DBG4 DBG4
#define DBG3 DBG3
#define DBG2 DBG2
#define DBG1 DBG1
#define DBG0 DBG0
#define DBG DBG
#define INFO INFO
#define WARN WARN
#define WARNING WARNING
#define ERR ERR
#define ERROR ERROR
#define CRITICAL CRITICAL
#define FATAL FATAL

#endif // MOXYGEN_USE_FOLLY
