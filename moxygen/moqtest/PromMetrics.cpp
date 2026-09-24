/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "moxygen/moqtest/PromMetrics.h"

#include <folly/FileUtil.h>
#include <folly/logging/xlog.h>

namespace moxygen {

std::string escapeLabelValue(const std::string& v) {
  std::string out;
  out.reserve(v.size());
  for (char c : v) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

void PromWriter::gauge(
    const std::string& name,
    const std::string& help,
    double value) {
  os_ << "# HELP " << name << " " << help << "\n"
      << "# TYPE " << name << " gauge\n"
      << name << "{" << labels_ << "} " << value << "\n";
}

void PromWriter::counter(
    const std::string& name,
    const std::string& help,
    uint64_t value) {
  os_ << "# HELP " << name << " " << help << "\n"
      << "# TYPE " << name << " counter\n"
      << name << "{" << labels_ << "} " << value << "\n";
}

// Buckets are cumulative over the run, so Prometheus rate() and
// histogram_quantile() work across scrapes.
void PromWriter::histogram(
    const std::string& name,
    const std::string& help,
    const std::vector<std::pair<std::string, LatencyHistogram>>& series) {
  os_ << "# HELP " << name << " " << help << "\n"
      << "# TYPE " << name << " histogram\n";
  for (const auto& [extra, hist] : series) {
    auto labels = extra.empty() ? labels_ : labels_ + "," + extra;
    auto cum = hist.cumulative();
    for (size_t i = 0; i < LatencyHistogram::kNumBounds; ++i) {
      double leSec = static_cast<double>(kLatencyBucketsMs[i]) / 1000.0;
      os_ << name << "_bucket{" << labels << ",le=\"" << leSec << "\"} "
          << cum[i] << "\n";
    }
    os_ << name << "_bucket{" << labels << ",le=\"+Inf\"} "
        << cum[LatencyHistogram::kNumBounds] << "\n";
    os_ << name << "_sum{" << labels << "} "
        << (static_cast<double>(hist.sum()) / 1000.0) << "\n";
    os_ << name << "_count{" << labels << "} " << hist.count() << "\n";
  }
}

void PromWriter::writeFile(const std::string& path) const {
  auto data = os_.str();
  try {
    // writeFileAtomic does the temp-write + rename the textfile collector
    // requires.
    folly::writeFileAtomic(path, folly::StringPiece(data));
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Failed to write metrics file " << path << ": " << ex.what();
  }
}

} // namespace moxygen
