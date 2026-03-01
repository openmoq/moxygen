/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/compat/Config.h>

#if MOXYGEN_USE_FOLLY
#include <folly/Unit.h>
#endif

namespace moxygen::compat {

#if MOXYGEN_USE_FOLLY
using Unit = folly::Unit;
constexpr auto unit = folly::unit;
#else
struct Unit {
  constexpr bool operator==(const Unit&) const {
    return true;
  }
  constexpr bool operator!=(const Unit&) const {
    return false;
  }
};
constexpr Unit unit{};
#endif

} // namespace moxygen::compat
