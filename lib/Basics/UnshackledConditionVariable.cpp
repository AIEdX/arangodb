////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "UnshackledConditionVariable.h"

using namespace arangodb;
using namespace arangodb::basics;

void UnshackledConditionVariable::notify_one() noexcept {
  {
    // acquire and release the mutex, in order to guarantee that the waiting
    // thread either has the update visible when checking the precondition,
    // or has already gone to sleep and released _mutex.
    auto lock = std::unique_lock(_mutex);
  }
  _cv.notify_one();
}

void UnshackledConditionVariable::notify_all() noexcept {
  {
    // acquire and release the mutex, in order to guarantee that the waiting
    // thread(s) either have the update visible when checking the precondition,
    // or have already gone to sleep and released _mutex.
    auto lock = std::unique_lock(_mutex);
  }
  _cv.notify_all();
}

void UnshackledConditionVariable::wait(
    std::unique_lock<UnshackledMutex>& lock) noexcept try {
  {
    auto guard = std::unique_lock(_mutex);
    // unlock before suspending the thread
    lock.unlock();
    _cv.wait(guard);
  }  // to prevent deadlocks, `guard` must not be locked while reacquiring
     // `lock`
  lock.lock();
} catch (...) {
  // We cannot handle any exceptions here.
  std::abort();
}

template<class Rep, class Period>
auto UnshackledConditionVariable::wait_for(
    std::unique_lock<UnshackledMutex>& lock,
    std::chrono::duration<Rep, Period> const& rel_time) noexcept
    -> std::cv_status {
  return wait_until(lock, std::chrono::steady_clock::now() + rel_time);
}

template<class Clock, class Duration>
auto UnshackledConditionVariable::wait_until(
    std::unique_lock<std::mutex>& lock,
    std::chrono::time_point<Clock, Duration> const& timeout_time) noexcept
    -> std::cv_status try {
  auto res = std::cv_status{};
  {
    auto guard = std::unique_lock(_mutex);
    // unlock before suspending the thread
    lock.unlock();
    res = _cv.wait_until(guard, timeout_time);
  }  // to prevent deadlocks, `guard` must not be locked while reacquiring
     // `lock`
  lock.lock();

  return res;
} catch (...) {
  // We cannot handle any exceptions here.
  std::abort();
}
