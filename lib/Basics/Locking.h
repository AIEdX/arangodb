////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/Common.h"

namespace arangodb::basics {

enum class LockerType {
  BLOCKING,  // always lock, blocking if the lock cannot be acquired instantly
  EVENTUAL,  // always lock, sleeping while the lock is not acquired
  TRY  // try to acquire the lock and give up instantly if it cannot be acquired
};

namespace ConditionalLocking {
static constexpr bool DoLock = true;
static constexpr bool DoNotLock = false;
}  // namespace ConditionalLocking

}  // namespace arangodb::basics
