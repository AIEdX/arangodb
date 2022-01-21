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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/Common.h"

#include "Basics/Mutex.h"
#include "Basics/system-functions.h"
#include "Cluster/ClusterInfo.h"
#include "Pregel/Reports.h"
#include "Pregel/Statistics.h"
#include "Scheduler/Scheduler.h"
#include "Utils/DatabaseGuard.h"
#include "PregelFeature.h"

#include <chrono>

namespace arangodb::pregel {

enum ExecutionState {
  DEFAULT = 0,  // before calling start
  RUNNING,      // during normal operation
  STORING,      // store results
  DONE,         // after everything is done
  CANCELED,     // after a terminal error or manual canceling
  IN_ERROR,     // after an error which should allow recovery
  RECOVERING,   // during recovery
  FATAL_ERROR   // execution can not continue because of errors
};
extern const char* ExecutionStateNames[8];

class PregelFeature;
class MasterContext;
class AggregatorHandler;
struct IAlgorithm;

struct Error {
  std::string message;
};

class Conductor : public std::enable_shared_from_this<Conductor> {
  friend class PregelFeature;

  ExecutionState _state = ExecutionState::DEFAULT;
  PregelFeature& _feature;
  std::chrono::system_clock::time_point _created;
  std::chrono::system_clock::time_point _expires;
  std::chrono::seconds _ttl = std::chrono::seconds(300);
  const DatabaseGuard _vocbaseGuard;
  const uint64_t _executionNumber;
  VPackBuilder _userParams;
  std::unique_ptr<IAlgorithm> _algorithm;
  mutable Mutex
      _callbackMutex;  // prevents concurrent calls to finishedGlobalStep

  std::vector<CollectionID> _vertexCollections;
  std::vector<CollectionID> _edgeCollections;
  std::vector<ServerID> _dbServers;
  std::vector<ShardID> _allShards;  // persistent shard list

  // maps from vertex collection name to a list of edge collections that this
  // vertex collection is restricted to. only use for a collection if there is
  // at least one entry for the collection!
  std::unordered_map<CollectionID, std::vector<CollectionID>>
      _edgeCollectionRestrictions;

  // initialized on startup
  std::unique_ptr<AggregatorHandler> _aggregators;
  std::unique_ptr<MasterContext> _masterContext;
  /// tracks the servers which responded, only used for stages where we expect
  /// a unique response, not necessarily during the async mode
  std::set<ServerID> _respondedServers;
  uint64_t _globalSuperstep = 0;
  /// adjustable maximum gss for some algorithms
  uint64_t _maxSuperstep = 500;
  /// determines whether we support async execution
  bool _asyncMode = false;
  bool _useMemoryMaps = false;
  bool _storeResults = false;
  bool _inErrorAbort = false;

  /// persistent tracking of active vertices, sent messages, runtimes and Pregel
  /// metrics
  StatsManager _statistics;
  ReportManager _reports;
  /// Current number of vertices
  uint64_t _totalVerticesCount = 0;
  uint64_t _totalEdgesCount = 0;
  /// some tracking info
  
  double _startTimeSecs = 0.0;
  double _computationStartTimeSecs = 0.0;
  TimePoint _global_start;
  std::optional<TimePoint> _computationStartTime{std::nullopt};
  double _finalizationStartTimeSecs = 0.0;
  std::optional<TimePoint> _storageStartTime;
  double _storeTimeSecs = 0.0;
  double _endTimeSecs = 0.0;
  double _stepStartTimeSecs = 0.0;  // start time of current gss
  TimePoint _gssStartTime;
  Scheduler::WorkHandle _workHandle;
  bool _return_extended_info = false;

  std::optional<Duration> _workerStartupDuration{std::nullopt};
  std::optional<Duration> _computationDuration{std::nullopt};
  Duration _storageDuration{0};
  Duration _gssDuration{0};

  bool _startGlobalStep();
  ErrorCode _initializeWorkers(std::string const& suffix,
                               VPackSlice additional);
  ErrorCode _finalizeWorkers();
  ErrorCode _sendToAllDBServers(std::string const& path,
                                VPackBuilder const& message);
  ErrorCode _sendToAllDBServers(std::string const& path,
                                VPackBuilder const& message,
                                std::function<void(VPackSlice)> handle);
  void _ensureUniqueResponse(VPackSlice body);

  // === REST callbacks ===
  /**
   * Checks only.
   * @param data
   */
  void finishedWorkerStartup(VPackSlice const& data);

  /**
   * Append reports (if any), accumulate statistics. If not all workers are
   * ready, return the empty VPackBuilder (in sync mode), otherwise (1) if
   * all messages have been received, set the new state depending on the
   * algorithm, aggregate the messages and return them together with
   * Utils::enterNextGSSKey set to true if necessary; (2) if messages are
   * being awaited, increase gss and enqueue starting a new global step.
   *
   * @param data
   * @return
   */
  VPackBuilder finishedWorkerStep(VPackSlice const& data);

  /**
   * Save and serialize statistics and reports, enqueue cleanupConductor.
   * @param data
   */
  void finishedWorkerFinalize(VPackSlice data);

  void finishedRecoveryStep(VPackSlice const& data);

  std::vector<ShardID> getShardIds(ShardID const& collection) const;

 public:
  Conductor(uint64_t executionNumber, TRI_vocbase_t& vocbase,
            std::vector<CollectionID> const& vertexCollections,
            std::vector<CollectionID> const& edgeCollections,
            std::unordered_map<std::string, std::vector<std::string>> const&
                edgeCollectionRestrictions,
            std::string const& algoName, VPackSlice const& userConfig,
            PregelFeature& feature);

  ~Conductor();

  /**
   * Set initial time, gss, set state to RUNNING, initialize workers.
   */
  void start();
  void cancel();
  void startRecovery();
  void collectAQLResults(velocypack::Builder& outBuilder, bool withId);
  void toVelocyPack(arangodb::velocypack::Builder& result) const;
  void setReturnExtendedInfo(bool b) {_return_extended_info = b;};

  double totalRuntimeSecs() const {
    return _endTimeSecs == 0.0 ? TRI_microtime() - _startTimeSecs
                               : _endTimeSecs - _startTimeSecs;
  }

  bool canBeGarbageCollected() const;

  uint64_t executionNumber() const { return _executionNumber; }

 private:
  void cancelNoLock();
  void updateState(ExecutionState state);
};
}  // namespace arangodb::pregel
