/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY
#include <folly/Try.h>
#else

#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace folly {

// Minimal std-mode replacement for folly::Try
// Holds either a value, an exception, or nothing

template <typename T>
class Try {
 public:
  Try() = default;

  // Construct with value
  /* implicit */ Try(T value) : storage_(std::move(value)) {}

  // Construct with exception
  /* implicit */ Try(std::exception_ptr ex) : storage_(std::move(ex)) {}

  // Check if has value
  bool hasValue() const {
    return std::holds_alternative<T>(storage_);
  }

  // Check if has exception
  bool hasException() const {
    return std::holds_alternative<std::exception_ptr>(storage_);
  }

  // Get value (throws if exception or empty)
  T& value() & {
    if (hasException()) {
      std::rethrow_exception(std::get<std::exception_ptr>(storage_));
    }
    if (!hasValue()) {
      throw std::runtime_error("Try is empty");
    }
    return std::get<T>(storage_);
  }

  const T& value() const& {
    if (hasException()) {
      std::rethrow_exception(std::get<std::exception_ptr>(storage_));
    }
    if (!hasValue()) {
      throw std::runtime_error("Try is empty");
    }
    return std::get<T>(storage_);
  }

  T&& value() && {
    if (hasException()) {
      std::rethrow_exception(std::get<std::exception_ptr>(storage_));
    }
    if (!hasValue()) {
      throw std::runtime_error("Try is empty");
    }
    return std::get<T>(std::move(storage_));
  }

  // Dereference operators
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T&& operator*() && { return std::move(value()); }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

  // Get exception pointer
  std::exception_ptr exception() const {
    if (hasException()) {
      return std::get<std::exception_ptr>(storage_);
    }
    return nullptr;
  }

  // Try to get exception of specific type
  template <typename E>
  const E* tryGetExceptionObject() const {
    if (!hasException()) {
      return nullptr;
    }
    try {
      std::rethrow_exception(std::get<std::exception_ptr>(storage_));
    } catch (const E& e) {
      return &e;
    } catch (...) {
      return nullptr;
    }
  }

  // Implicit bool conversion
  explicit operator bool() const {
    return hasValue();
  }

 private:
  std::variant<std::monostate, T, std::exception_ptr> storage_;
};

// Specialization for void
template <>
class Try<void> {
 public:
  Try() : hasValue_(true) {}

  explicit Try(std::exception_ptr ex) : exception_(std::move(ex)) {}

  bool hasValue() const {
    return hasValue_ && !exception_;
  }

  bool hasException() const {
    return exception_ != nullptr;
  }

  void value() const {
    if (hasException()) {
      std::rethrow_exception(exception_);
    }
    if (!hasValue_) {
      throw std::runtime_error("Try is empty");
    }
  }

  std::exception_ptr exception() const {
    return exception_;
  }

  template <typename E>
  const E* tryGetExceptionObject() const {
    if (!hasException()) {
      return nullptr;
    }
    try {
      std::rethrow_exception(exception_);
    } catch (const E& e) {
      return &e;
    } catch (...) {
      return nullptr;
    }
  }

  explicit operator bool() const {
    return hasValue();
  }

 private:
  bool hasValue_{false};
  std::exception_ptr exception_;
};

// Factory function
template <typename T>
Try<T> makeTryWith(T value) {
  return Try<T>(std::move(value));
}

inline Try<void> makeTryWith() {
  return Try<void>();
}

} // namespace folly

#endif // !MOXYGEN_USE_FOLLY
