/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/mlog/FileMLogger.h"
#include <folly/json/json.h>
#include <folly/logging/xlog.h>
#include <fstream>
#include <sstream>

namespace moxygen {

std::string FileMLogger::derivePath() const {
  std::string path;
  if (dir_) {
    // Try DCID first, then fall back to server's own CID (srcCid), otherwise
    // use path_
    if (dcid_) {
      std::string dcidHex = dcid_->hex();
      if (!dcidHex.empty()) {
        path = *dir_ + "/" + dcidHex + ".mlog";
      }
    }
    if (path.empty() && srcCid_) {
      std::string srcCidHex = srcCid_->hex();
      if (!srcCidHex.empty()) {
        path = *dir_ + "/" + srcCidHex + ".mlog";
      }
    }
    if (path.empty()) {
      path = path_;
    }
    // Fallback to explicit path, or "unknown.mlog" if all identifiers missing
    if (path.empty()) {
      path = *dir_ + "/unknown.mlog";
    }
  } else {
    path = path_;
  }
  return path;
}

void FileMLogger::outputLogs() {
  std::string path = derivePath();

  if (writeExecutor_) {
    // Pre-format logs on the calling thread (where formatLog() is safe to
    // call), then write the serialized strings asynchronously to avoid blocking
    // the event loop.
    std::vector<std::string> serialized;
    serialized.reserve(logs_.size());
    for (const auto& log : logs_) {
      serialized.push_back(folly::toPrettyJson(formatLog(log)));
    }
    writeExecutor_->add([p = path, lines = std::move(serialized)]() {
      std::ofstream fileObj(p);
      for (const auto& line : lines) {
        fileObj << line << '\n';
      }
    });
  } else {
    std::ofstream fileObj(path);
    for (const auto& log : logs_) {
      fileObj << folly::toPrettyJson(formatLog(log)) << '\n';
    }
  }
}

} // namespace moxygen
