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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ApplicationFeatures/ApplicationFeature.h"
#include "Basics/Thread.h"
#include "Containers/FlatHashMap.h"
#include "Containers/FlatHashSet.h"
#include "Replication2/Version.h"
#include "RestServer/arangod.h"
#include "Utils/DatabaseGuard.h"
#include "Utils/VersionTracker.h"
#include "VocBase/voc-types.h"
#include "VocBase/Methods/Databases.h"

#include <mutex>
#include <memory>
#include <vector>

struct TRI_vocbase_t;

namespace arangodb {
namespace application_features {
class ApplicationServer;
}
class IOHeartbeatThread;
class LogicalCollection;
}  // namespace arangodb

namespace arangodb::velocypack {
class Builder;
class Slice;
}  // namespace arangodb::velocypack

namespace arangodb {

class DatabaseManagerThread final : public ServerThread<ArangodServer> {
 public:
  DatabaseManagerThread(DatabaseManagerThread const&) = delete;
  DatabaseManagerThread& operator=(DatabaseManagerThread const&) = delete;

  explicit DatabaseManagerThread(Server&);
  ~DatabaseManagerThread();

  void run() override;

 private:
  // how long will the thread pause between iterations
  static constexpr unsigned long waitTime() {
    return static_cast<unsigned long>(500U * 1000U);
  }
};

class DatabaseFeature : public ArangodFeature {
  friend class DatabaseManagerThread;

 public:
  static constexpr std::string_view name() noexcept { return "Database"; }

  explicit DatabaseFeature(Server& server);
  ~DatabaseFeature();

  void collectOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void validateOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void start() override final;
  void beginShutdown() override final;
  void stop() override final;
  void unprepare() override final;
  void prepare() override final;

  // used by unit tests
#ifdef ARANGODB_USE_GOOGLE_TESTS
  ErrorCode loadDatabases(velocypack::Slice databases) {
    return iterateDatabases(databases);
  }
#endif

  /// @brief will be called when the recovery phase has run
  /// this will call the engine-specific recoveryDone() procedures
  /// and will execute engine-unspecific operations (such as starting
  /// the replication appliers) for all databases
  void recoveryDone();

  /// @brief whether or not the DatabaseFeature has started (and thus has
  /// completely populated its lists of databases and collections from
  /// persistent storage)
  bool started() const noexcept;

  /// @brief enumerate all databases
  void enumerate(std::function<void(TRI_vocbase_t*)> const& callback);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief register a callback
  ///        if StorageEngine.inRecovery() -> call at start of recoveryDone()
  ///                                         and fail recovery if callback
  ///                                         !ok()
  ///        if !StorageEngine.inRecovery() -> call immediately and return
  ///                                          result
  //////////////////////////////////////////////////////////////////////////////
  Result registerPostRecoveryCallback(std::function<Result()>&& callback);

  VersionTracker* versionTracker() { return &_versionTracker; }

  /// @brief get the ids of all local databases
  std::vector<TRI_voc_tick_t> getDatabaseIds(bool includeSystem);
  std::vector<std::string> getDatabaseNames();
  std::vector<std::string> getDatabaseNamesForUser(std::string const& user);

  Result createDatabase(arangodb::CreateDatabaseInfo&&, TRI_vocbase_t*& result);

  ErrorCode dropDatabase(std::string_view name, bool removeAppsDirectory);
  ErrorCode dropDatabase(TRI_voc_tick_t id, bool removeAppsDirectory);

  void inventory(arangodb::velocypack::Builder& result, TRI_voc_tick_t,
                 std::function<bool(arangodb::LogicalCollection const*)> const&
                     nameFilter);

  VocbasePtr useDatabase(std::string_view name) const;
  VocbasePtr useDatabase(TRI_voc_tick_t id) const;

  TRI_vocbase_t* lookupDatabase(std::string_view name) const;
  void enumerateDatabases(
      std::function<void(TRI_vocbase_t& vocbase)> const& func);
  std::string translateCollectionName(std::string_view dbName,
                                      std::string_view collectionName);

  bool ignoreDatafileErrors() const { return _ignoreDatafileErrors; }
  bool isInitiallyEmpty() const { return _isInitiallyEmpty; }
  bool checkVersion() const { return _checkVersion; }
  bool upgrade() const { return _upgrade; }
  bool waitForSync() const { return _defaultWaitForSync; }
  replication::Version defaultReplicationVersion() const {
    return _defaultReplicationVersion;
  }

  /// @brief whether or not extended names for databases can be used
  bool extendedNamesForDatabases() const { return _extendedNamesForDatabases; }
  /// @brief will be called only during startup when reading stored value from
  /// storage engine
  void extendedNamesForDatabases(bool value) {
    _extendedNamesForDatabases = value;
  }

  /// @brief currently always false, until feature is implemented
  bool extendedNamesForCollections() const { return false; }
  /// @brief currently always false, until feature is implemented
  bool extendedNamesForViews() const { return false; }
  /// @brief currently always false, until feature is implemented
  bool extendedNamesForAnalyzers() const { return false; }

  void enableCheckVersion() { _checkVersion = true; }
  void enableUpgrade() { _upgrade = true; }
  void disableUpgrade() { _upgrade = false; }
  void isInitiallyEmpty(bool value) { _isInitiallyEmpty = value; }

  static TRI_vocbase_t& getCalculationVocbase();

 private:
  static void initCalculationVocbase(ArangodServer& server);

  void stopAppliers();

  /// @brief create base app directory
  ErrorCode createBaseApplicationDirectory(std::string const& appPath,
                                           std::string const& type);

  /// @brief create app subdirectory for a database
  ErrorCode createApplicationDirectory(std::string const& name,
                                       std::string const& basePath,
                                       bool removeExisting);

  /// @brief iterate over all databases in the databases directory and open them
  ErrorCode iterateDatabases(velocypack::Slice const& databases);

  /// @brief close all opened databases
  void closeOpenDatabases();

  /// @brief close all dropped databases
  void closeDroppedDatabases();

  void verifyAppPaths();

  /// @brief activates deadlock detection in all existing databases
  void enableDeadlockDetection();

  bool _defaultWaitForSync{false};
  bool _ignoreDatafileErrors{false};
  bool _isInitiallyEmpty{false};
  bool _checkVersion{false};
  bool _upgrade{false};
  // allow extended database names or not
  bool _extendedNamesForDatabases{false};
  bool _performIOHeartbeat{true};
  std::atomic<bool> _started{false};

  replication::Version _defaultReplicationVersion{replication::Version::ONE};

  std::unique_ptr<DatabaseManagerThread> _databaseManager;
  std::unique_ptr<IOHeartbeatThread> _ioHeartbeatThread;

  using DatabasesList = containers::FlatHashMap<std::string, TRI_vocbase_t*>;
  class DatabasesListGuard {
   public:
    [[nodiscard]] static std::shared_ptr<DatabasesList> create() {
      return std::make_shared<DatabasesList>();
    }

    [[nodiscard]] auto load() const noexcept {
      auto lists = std::atomic_load(&_impl);
      TRI_ASSERT(lists != nullptr);
      return lists;
    }

    [[nodiscard]] static auto make(
        std::shared_ptr<DatabasesList const> const& lists) {
      return std::make_shared<DatabasesList>(*lists);
    }

    void store(std::shared_ptr<DatabasesList const>&& lists) noexcept {
      TRI_ASSERT(lists != nullptr);
      std::atomic_store(&_impl, std::move(lists));
    }

   private:
    // TODO(MBkkt) replace via std::atomic<std::shared_ptr>
    //  when libc++ support it or we drop it support
    std::shared_ptr<DatabasesList const> _impl = create();
  } _databases;
  mutable std::mutex _databasesMutex;
  containers::FlatHashSet<TRI_vocbase_t*> _droppedDatabases;

  /// @brief lock for serializing the creation of databases
  std::mutex _databaseCreateLock;

  std::vector<std::function<Result()>> _pendingRecoveryCallbacks;

  /// @brief a simple version tracker for all database objects
  /// maintains a global counter that is increased on every modification
  /// (addition, removal, change) of database objects
  VersionTracker _versionTracker;
};

}  // namespace arangodb
