////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2022 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////
#pragma once
#include "RocksDBEngine/RocksDBPersistedLog.h"

namespace arangodb::replication2::test {

struct ThreadAsyncExecutor : RocksDBAsyncLogWriteBatcher::IAsyncExecutor {
  using Func = fu2::unique_function<void() noexcept>;

  void operator()(Func fn) override;

  ~ThreadAsyncExecutor() override;
  ThreadAsyncExecutor();

 private:
  void run() noexcept;

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<Func> queue;
  bool stopping{false};
  // initialize thread last, so the thread cannot access uninitialized members
  std::thread thread;
};

struct SyncExecutor : RocksDBAsyncLogWriteBatcher::IAsyncExecutor {
  void operator()(fu2::unique_function<void() noexcept> f) noexcept override {
    std::move(f).operator()();
  }
};
}  // namespace arangodb::replication2::test
