// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// Base test class, with various utility functions.
#ifndef KUDU_UTIL_TEST_UTIL_H
#define KUDU_UTIL_TEST_UTIL_H

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include <gtest/gtest.h>

#include "kudu/gutil/port.h"
#include "kudu/util/monotime.h"

#define SKIP_IF_SLOW_NOT_ALLOWED() do { \
  if (!AllowSlowTests()) { \
    GTEST_SKIP() << "test is skipped; set KUDU_ALLOW_SLOW_TESTS=1 to run"; \
  } \
} while (0)

#define ASSERT_EVENTUALLY(expr) do { \
  AssertEventually(expr); \
  NO_PENDING_FATALS(); \
} while (0)

namespace google {
class FlagSaver;
} // namespace google

namespace kudu {

class Env;
class Status;

extern const char* kInvalidPath;

class KuduTestEventListener : public ::testing::EmptyTestEventListener {
  void OnTestIterationStart(const testing::UnitTest& unit_test, int iteration) override;
};

class KuduTest : public ::testing::Test {
 public:
  KuduTest();

  ~KuduTest() override;

  void SetUp() override;

  // Tests assume that they run with no outside-provided kerberos credentials, and if the
  // user happened to have some credentials available they might fail due to being already
  // kinitted to a different realm, etc. This function overrides the relevant environment
  // variables so that we don't pick up the user's credentials.
  static void OverrideKrb5Environment();

  // Returns the encryption tenant name, tenant id, key, IV, and version used by the test.
  static void GetEncryptionKey(std::string* name, std::string* id,
                               std::string* key, std::string* iv, std::string* version);

 protected:
  // Sets the flags to enable encryption if 'enable_encryption' is true.
  static void SetEncryptionFlags(bool enable_encryption);

  // Returns absolute path based on a unit test-specific work directory, given
  // a relative path. Useful for writing test files that should be deleted after
  // the test ends.
  std::string GetTestPath(const std::string& relative_path) const;

  Env* env_;

  // Reset flags on every test. Allocated on the heap so it can be destroyed
  // (and the flags reset) before test_dir_ is deleted.
  std::unique_ptr<google::FlagSaver> flag_saver_;

  std::string test_dir_;
};

// Returns true if slow tests are runtime-enabled.
bool AllowSlowTests();

// Returns true if the KUDU_USE_LARGE_KEYS_IN_TESTS environment variable is set
// to true. This is required to pass certain tests in FIPS approved mode.
bool UseLargeKeys();

bool EnableEncryption();

// Override the given gflag to the new value, only in the case that
// slow tests are enabled and the user hasn't otherwise overridden
// it on the command line.
// Example usage:
//
// OverrideFlagForSlowTests(
//     "client_inserts_per_thread",
//     strings::Substitute("$0", FLAGS_client_inserts_per_thread * 100));
//
void OverrideFlagForSlowTests(const std::string& flag_name,
                              const std::string& new_value);

// Call srand() with a random seed based on the current time, reporting
// that seed to the logs. The time-based seed may be overridden by passing
// --test_random_seed= from the CLI in order to reproduce a failed randomized
// test. Returns the seed.
int SeedRandom();

// Return a per-test directory in which to store test data. Guaranteed to
// return the same directory every time for a given unit test.
//
// May only be called from within a gtest unit test. Prefer KuduTest::test_dir_
// if a KuduTest instance is available.
std::string GetTestDataDirectory();

// Returns a unique socket path for use in tests in the form of:
//   <test-dir>/<name>-<uuid>.sock
//
// The path is in the base test directory because the path to Unix domain
// socket file cannot be longer than ~100 bytes.
std::string GetTestSocketPath(const std::string& name);

// Return the directory which contains the test's executable.
std::string GetTestExecutableDirectory();

// Wait until 'f()' succeeds without adding any GTest 'fatal failures'.
// For example:
//
//   AssertEventually([]() {
//     ASSERT_GT(ReadValueOfMetric(), 10);
//   });
//
// The function is run in a loop with optional back-off.
//
// To check whether AssertEventually() eventually succeeded, call
// NO_PENDING_FATALS() afterward, or use ASSERT_EVENTUALLY() which performs
// this check automatically.
enum class AssertBackoff {
  // Use exponential back-off while looping, capped at one second.
  EXPONENTIAL,

  // Sleep for a millisecond while looping.
  NONE,
};
void AssertEventually(const std::function<void(void)>& f,
                      const MonoDelta& timeout = MonoDelta::FromSeconds(30),
                      AssertBackoff backoff = AssertBackoff::EXPONENTIAL);

// Count the number of open file descriptors in use by this process.
// 'path_pattern' is a glob-style pattern. Only paths that match this
// pattern are included. Note that '*' in this pattern is recursive
// unlike the usual behavior of path globs.
int CountOpenFds(Env* env, const std::string& path_pattern);

// Waits for the subprocess to bind to any listening TCP port at least on one
// of the provided IP addresses and returns the port number. As for values
// for the 'addresses' parameter, {} (an empty vector) and { "0.0.0.0" }
// semantically mean the same.
Status WaitForTcpBind(pid_t pid, uint16_t* port,
                      const std::vector<std::string>& addresses,
                      MonoDelta timeout) WARN_UNUSED_RESULT;

// Similar to above but binds to any listening UDP port.
Status WaitForUdpBind(pid_t pid, uint16_t* port,
                      const std::vector<std::string>& addresses,
                      MonoDelta timeout) WARN_UNUSED_RESULT;

// Similar to WaitForTcpBind(), but when port is known beforehand
// and the PID doesn't matter.
Status WaitForTcpBindAtPort(const std::vector<std::string>& addresses,
                            uint16_t port,
                            MonoDelta timeout) WARN_UNUSED_RESULT;

// Similar to WaitForUdpBind(), but when port is known beforehand
// and the PID doesn't matter.
Status WaitForUdpBindAtPort(const std::vector<std::string>& addresses,
                            uint16_t port,
                            MonoDelta timeout) WARN_UNUSED_RESULT;

// Find the home directory of a Java-style application, e.g. JAVA_HOME or
// HADOOP_HOME.
//
// Checks the environment, or falls back to a symlink in the bin installation
// directory.
Status FindHomeDir(const std::string& name,
                   const std::string& bin_dir,
                   std::string* home_dir) WARN_UNUSED_RESULT;

// Contains the endpoints which are to be found on Master and TServer as well.
// Key: endpoint name, value: content-type header for that particular endpoint.
const std::unordered_map<std::string, std::string>& GetCommonWebserverEndpoints();

// Contains the endpoints which are to be found on TServer only.
// Key: endpoint name, value: content-type header for that particular endpoint.
const std::unordered_map<std::string, std::string>& GetTServerWebserverEndpoints(
    const std::string& tablet_id);

// Contains the endpoints which are to be found on Master only.
// Key: endpoint name, value: content-type header for that particular endpoint.
const std::unordered_map<std::string, std::string>& GetMasterWebserverEndpoints(
    const std::string& table_id);

// Prometheus metrics output sanity check. This check is used in both the tablet_server-test
// and the master-test, so it is placed here.
void CheckPrometheusOutput(const std::string& prometheus_output);
} // namespace kudu
#endif
