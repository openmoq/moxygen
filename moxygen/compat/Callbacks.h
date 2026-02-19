/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Unit.h>
#include <functional>
#include <memory>

namespace moxygen::compat {

// Generic result callback for async operations
template <typename T, typename E>
class ResultCallback {
 public:
  virtual ~ResultCallback() = default;
  virtual void onSuccess(T value) = 0;
  virtual void onError(E error) = 0;
};

// Specialization for void/Unit return type
template <typename E>
class ResultCallback<Unit, E> {
 public:
  virtual ~ResultCallback() = default;
  virtual void onSuccess() = 0;
  virtual void onError(E error) = 0;
};

// Ready callback for backpressure signaling
template <typename E>
class ReadyCallback {
 public:
  virtual ~ReadyCallback() = default;
  virtual void onReady(uint64_t availableBytes) = 0;
  virtual void onError(E error) = 0;
};

// Stream credit callback
template <typename E>
class StreamCreditCallback {
 public:
  virtual ~StreamCreditCallback() = default;
  virtual void onCreditAvailable() = 0;
  virtual void onError(E error) = 0;
};

// Lambda-based callback wrappers for convenience
template <typename T, typename E>
class LambdaResultCallback : public ResultCallback<T, E> {
 public:
  LambdaResultCallback(
      std::function<void(T)> onSuccess,
      std::function<void(E)> onError)
      : onSuccess_(std::move(onSuccess)), onError_(std::move(onError)) {}

  void onSuccess(T value) override { onSuccess_(std::move(value)); }
  void onError(E error) override { onError_(std::move(error)); }

 private:
  std::function<void(T)> onSuccess_;
  std::function<void(E)> onError_;
};

template <typename E>
class LambdaResultCallback<Unit, E> : public ResultCallback<Unit, E> {
 public:
  LambdaResultCallback(
      std::function<void()> onSuccess,
      std::function<void(E)> onError)
      : onSuccess_(std::move(onSuccess)), onError_(std::move(onError)) {}

  void onSuccess() override { onSuccess_(); }
  void onError(E error) override { onError_(std::move(error)); }

 private:
  std::function<void()> onSuccess_;
  std::function<void(E)> onError_;
};

// Factory functions
template <typename T, typename E>
std::shared_ptr<ResultCallback<T, E>> makeResultCallback(
    std::function<void(T)> onSuccess,
    std::function<void(E)> onError) {
  return std::make_shared<LambdaResultCallback<T, E>>(
      std::move(onSuccess), std::move(onError));
}

template <typename E>
std::shared_ptr<ResultCallback<Unit, E>> makeResultCallback(
    std::function<void()> onSuccess,
    std::function<void(E)> onError) {
  return std::make_shared<LambdaResultCallback<Unit, E>>(
      std::move(onSuccess), std::move(onError));
}

} // namespace moxygen::compat
