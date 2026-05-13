/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moxygen/mlog/FileMLogger.h>

#include <folly/executors/ManualExecutor.h>
#include <folly/portability/GTest.h>
#include <quic/codec/QuicConnectionId.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace moxygen {

class FileMLoggerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Each test gets an isolated temp dir derived from testing::TempDir()
    dir_ = fs::path(testing::TempDir()) / "mlog_test";
    fs::create_directories(dir_);
  }

  void TearDown() override {
    fs::remove_all(dir_);
  }

  // Helper: create a ConnectionId from fixed bytes
  static quic::ConnectionId makeCid(std::vector<uint8_t> bytes) {
    return quic::ConnectionId::createAndMaybeCrash(std::move(bytes));
  }

  fs::path dir_;
};

// ---------------------------------------------------------------------------
// Sync write tests
// ---------------------------------------------------------------------------

TEST_F(FileMLoggerTest, SyncWrite_CreatesFile) {
  const auto kSyncOutputFile = "sync_out.mlog";
  auto path = (dir_ / kSyncOutputFile).string();
  FileMLogger logger(VantagePoint::SERVER);
  logger.setPath(path);
  logger.outputLogs();

  EXPECT_TRUE(fs::exists(path));
}

TEST_F(FileMLoggerTest, SyncWrite_ErrorOnBadPath) {
  // Write to a path whose parent dir does not exist — should not throw
  FileMLogger logger(VantagePoint::SERVER);
  logger.setPath("/nonexistent_dir_xyz/out.mlog");
  EXPECT_NO_THROW(logger.outputLogs());
}

// ---------------------------------------------------------------------------
// Async write tests
// ---------------------------------------------------------------------------

TEST_F(FileMLoggerTest, AsyncWrite_FileNotCreatedBeforeDrain) {
  const auto kAsyncOutputFile = "async_out.mlog";
  auto executor = std::make_shared<folly::ManualExecutor>();
  auto path = (dir_ / kAsyncOutputFile).string();

  FileMLogger logger(VantagePoint::SERVER);
  logger.setPath(path);
  logger.setWriteExecutor(executor);
  logger.outputLogs();

  // Task is enqueued but not yet run
  EXPECT_FALSE(fs::exists(path));

  executor->drain();
  EXPECT_TRUE(fs::exists(path));
}

TEST_F(FileMLoggerTest, AsyncWrite_ErrorOnBadPathDoesNotThrow) {
  auto executor = std::make_shared<folly::ManualExecutor>();
  FileMLogger logger(VantagePoint::SERVER);
  logger.setPath("/nonexistent_dir_xyz/out.mlog");
  logger.setWriteExecutor(executor);
  logger.outputLogs();
  EXPECT_NO_THROW(executor->drain());
}

// ---------------------------------------------------------------------------
// derivePath tests (verified through outputLogs file creation)
// ---------------------------------------------------------------------------

// When dir + dcid set: output is {dir}/{dcid_hex}.mlog
TEST_F(FileMLoggerTest, DerivePath_DcidTakesPrecedence) {
  const auto kDcidFile = "12345678.mlog";
  const std::vector<uint8_t> kTestDcid = {0x12, 0x34, 0x56, 0x78};
  FileMLogger logger(VantagePoint::SERVER);
  logger.setDir(dir_.string());
  logger.setDcid(makeCid(kTestDcid));

  logger.outputLogs();

  EXPECT_TRUE(fs::exists(dir_ / kDcidFile));
}

// When dir + no dcid + srcCid set: output is {dir}/{srcCid_hex}.mlog
TEST_F(FileMLoggerTest, DerivePath_FallsBackToSrcCid) {
  const auto kSrcCidFile = "abcd.mlog";
  const std::vector<uint8_t> kTestSrcCid = {0xAB, 0xCD};
  FileMLogger logger(VantagePoint::SERVER);
  logger.setDir(dir_.string());
  logger.setSrcCid(makeCid(kTestSrcCid));
  // dcid deliberately not set

  logger.outputLogs();

  EXPECT_TRUE(fs::exists(dir_ / kSrcCidFile));
}

// When dir + no cids + explicit path_: output is the explicit path (not dir-prefixed)
TEST_F(FileMLoggerTest, DerivePath_FallsBackToExplicitPath) {
  const auto kExplicitFile = "explicit_test.mlog";
  auto explicitPath = (dir_ / kExplicitFile).string();
  FileMLogger logger(VantagePoint::SERVER);
  logger.setDir(dir_.string());
  logger.setPath(explicitPath);
  // No cids set

  logger.outputLogs();

  EXPECT_TRUE(fs::exists(explicitPath));
}

// When no dir set: output is the explicit path regardless of any cids
TEST_F(FileMLoggerTest, DerivePath_NoDirUsesExplicitPath) {
  const auto kNoOpFile = "nodir_test.mlog";
  const std::vector<uint8_t> kTestCidSimple = {0x01, 0x02};
  auto path = (dir_ / kNoOpFile).string();
  FileMLogger logger(VantagePoint::SERVER);
  logger.setPath(path);
  logger.setDcid(makeCid(kTestCidSimple)); // dcid present but dir is not set

  logger.outputLogs();

  // Should write to the explicit path, not a dcid-named file
  EXPECT_TRUE(fs::exists(path));
  EXPECT_FALSE(fs::exists(dir_ / "dcid_derived.mlog"));
}

// When dir set + dcid takes precedence over srcCid
TEST_F(FileMLoggerTest, DerivePath_DcidTakesPrecedenceOverSrcCid) {
  const auto kDcidPrecedenceFile = "aabbccdd.mlog";
  const auto kSrcCidPrecedenceFile = "eeff0011.mlog";
  const std::vector<uint8_t> kTestCidDead = {0xAA, 0xBB, 0xCC, 0xDD};
  const std::vector<uint8_t> kTestCidBeef = {0xEE, 0xFF, 0x00, 0x11};
  FileMLogger logger(VantagePoint::SERVER);
  logger.setDir(dir_.string());
  logger.setDcid(makeCid(kTestCidDead));
  logger.setSrcCid(makeCid(kTestCidBeef));

  logger.outputLogs();

  EXPECT_TRUE(fs::exists(dir_ / kDcidPrecedenceFile));
  EXPECT_FALSE(fs::exists(dir_ / kSrcCidPrecedenceFile));
}

} // namespace moxygen
