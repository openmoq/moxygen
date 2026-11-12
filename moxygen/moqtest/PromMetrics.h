/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "moxygen/moqtest/LatencyHistogram.h"

namespace moxygen {

std::string escapeLabelValue(const std::string& v);

// Builds one Prometheus text-format file for a node_exporter textfile
// collector.  Every series carries the base labels.
class PromWriter {
 public:
  explicit PromWriter(std::string labels) : labels_(std::move(labels)) {}

  void gauge(const std::string& name, const std::string& help, double value);
  void
  counter(const std::string& name, const std::string& help, uint64_t value);

  // Latency in seconds.  Each series pairs a histogram with extra labels,
  // which are appended to the base labels.
  void histogram(
      const std::string& name,
      const std::string& help,
      const std::vector<std::pair<std::string, LatencyHistogram>>& series);

  // Logs and swallows write errors, so a full disk doesn't end the run.
  void writeFile(const std::string& path) const;

 private:
  std::string labels_;
  std::ostringstream os_;
};

} // namespace moxygen
