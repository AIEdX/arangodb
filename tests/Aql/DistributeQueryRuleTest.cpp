////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include "Aql/Query.h"
#include "Aql/ClusterNodes.h"
#include "Aql/CollectNode.h"
#include "Aql/PlanSnippet.h"
#include "Aql/SortNode.h"
#include "Transaction/StandaloneContext.h"
#include "Basics/VelocyPackHelper.h"

#include "../Mocks/Servers.h"

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::transaction;

namespace arangodb {
namespace tests {
namespace aql {

#define useOptimize 1

namespace {

struct NodeNamePrinter
    : public arangodb::aql::WalkerWorkerBase<arangodb::aql::ExecutionNode> {
  void after(arangodb::aql::ExecutionNode* n) override {
    if (!_result.empty()) {
      _result += ", ";
    }
    _result += n->getTypeString();
  }

  std::string result() const { return _result; }

 private:
  std::string _result{};
};

std::string nodeNames(ExecutionPlan const& plan) {
  NodeNamePrinter walker{};
  plan.root()->walk(walker);
  return walker.result();
}

std::string nodeNames(std::vector<std::string> const& nodes) {
  std::string result;
  for (auto const& n : nodes) {
    if (!result.empty()) {
      result += ", ";
    }
    result += n;
  }
  return result;
}

struct NodeTypeAsserter
    : public arangodb::aql::WalkerWorkerBase<arangodb::aql::ExecutionNode> {
  NodeTypeAsserter(ExecutionPlan const& plan,
                   std::vector<std::string> const& expectedNodes)
      : arangodb::aql::WalkerWorkerBase<arangodb::aql::ExecutionNode>(),
        _plan(plan),
        _expectedNodes(expectedNodes) {}

  void after(arangodb::aql::ExecutionNode* n) override {
    if (_index >= _expectedNodes.size()) {
      ASSERT_TRUE(false) << "superflous node at position #" << _index << "\n"
                         << "found :" << n->getTypeString()
                         << "Actual:   " << nodeNames(_plan) << "\n"
                         << "Expected: " << nodeNames(_expectedNodes) << "\n";
      _index++;
      return;
    }
    ASSERT_EQ(n->getTypeString(), _expectedNodes[_index])
        << "Unequal node at position #" << _index << "\n"
        << "Actual:   " << nodeNames(_plan) << "\n"
        << "Expected: " << nodeNames(_expectedNodes) << "\n";
    _index++;
  }

  size_t numberNodes() { return _index; }

 private:
  size_t _index{0};
  ExecutionPlan const& _plan;
  std::vector<std::string> const& _expectedNodes;
};

std::string nodeNames(VPackSlice nodes) {
  std::string result;
  for (auto n : VPackArrayIterator(nodes)) {
    if (!result.empty()) {
      result += ", ";
    }
    result += n.get("type").copyString();
  }
  return result;
}

struct BasicPlanRulesAsserter
    : public arangodb::aql::WalkerWorkerBase<arangodb::aql::ExecutionNode> {
  bool before(ExecutionNode* node) {
    auto plan = node->getPlanSnippet();
    // Every Node needs to have a plan
    if (plan == nullptr) {
      EXPECT_NE(plan, nullptr)
          << "Node (" << node->id() << ") " << node->getTypeString()
          << " is not assigned a server position!";
    } else {
      // Every Plan needs to follow the switching pattern
      // We can only switch from Coordinator to DBServer
      // and from DBServer back to Coordinator, no exceptions
      EXPECT_EQ(plan->isOnCoordinator(), _hasToBeOnCoordinator);
    }
    if (!_switchToNextPlan) {
      EXPECT_EQ(plan, _lastPlan)
          << "We switched to a new plan, without doing the Remote dance";
    }

    // Some Nodes have specific position requirements
    switch (node->getType()) {
      case ExecutionNode::INSERT:
      case ExecutionNode::UPDATE:
      case ExecutionNode::REMOVE:
      case ExecutionNode::REPLACE:
      case ExecutionNode::UPSERT:
      case ExecutionNode::ENUMERATE_COLLECTION:
      case ExecutionNode::INDEX:
      case ExecutionNode::ENUMERATE_IRESEARCH_VIEW:
      case ExecutionNode::MATERIALIZE: {
        // Have to be on DBServer
        EXPECT_FALSE(_hasToBeOnCoordinator);
        break;
      }
      case ExecutionNode::TRAVERSAL: {
        // Not handled yet
        TRI_ASSERT(false);
        break;
      }
      case ExecutionNode::SUBQUERY_END: {
        _openSubqueries.emplace_back(plan);
        break;
      }
      case ExecutionNode::SUBQUERY_START: {
        auto endPlan = _openSubqueries.back();
        _openSubqueries.pop_back();
        EXPECT_EQ(endPlan->isOnCoordinator(), plan->isOnCoordinator())
            << "Subquery start/end are not planned on the same server type";
        if (!_hasToBeOnCoordinator) {
          // If we are not on the Coordinator, then both SubqueryStart and
          // SubqueryEnd have to be on the same Plan
          EXPECT_EQ(endPlan, plan);
        }
        break;
      }

      default: {
        // Nothing to Check
        break;
      }
    }

    if (node->getType() == ExecutionNode::REMOTE) {
      // If we hit a remote the Next Plan has to be new.
      // and we need to be on the other type of server
      _switchToNextPlan = true;
      _hasToBeOnCoordinator = !_hasToBeOnCoordinator;
    }

    _lastPlan = plan;
    return false;
  }

 private:
  bool _switchToNextPlan{true};
  bool _hasToBeOnCoordinator{true};
  std::shared_ptr<PlanSnippet> _lastPlan;
  std::vector<std::shared_ptr<PlanSnippet>> _openSubqueries;
};

}  // namespace

class DistributeQueryRuleTest : public ::testing::Test {
 protected:
  static inline std::unique_ptr<mocks::MockCoordinator> server;

  std::unique_ptr<ExecutionPlan> optimizeQuery(std::string const& queryString) {
    auto ctx = std::make_shared<StandaloneContext>(server->getSystemDatabase());
    auto const bindParamVPack = VPackParser::fromJson("{}");
#if useOptimize
    auto const optionsVPack = VPackParser::fromJson(
        R"json({"optimizer": {"rules": ["insert-distribute-calculations", "distribute-query"]}})json");
#else
    auto const optionsVPack = VPackParser::fromJson(R"json({})json");
#endif
    _query = Query::create(ctx, QueryString(queryString), bindParamVPack,
                           QueryOptions(optionsVPack->slice()));
    auto plan = _query->getOptimizedPlan();
    assertPositionsAreValid(plan.get());
    return plan;
  }

  std::shared_ptr<VPackBuilder> explainQuery(std::string const& queryString) {
    auto ctx = std::make_shared<StandaloneContext>(server->getSystemDatabase());
    auto const bindParamVPack = VPackParser::fromJson("{}");
#if useOptimize
    auto const optionsVPack = VPackParser::fromJson(
        R"json({"optimizer": {"rules": ["insert-distribute-calculations", "distribute-query"]}})json");
#else
    auto const optionsVPack = VPackParser::fromJson(R"json({})json");
#endif
    _query = Query::create(ctx, QueryString(queryString), bindParamVPack,
                           QueryOptions(optionsVPack->slice()));

    // NOTE: We can only get a VPacked Variant of the Plan, we cannot inject
    // deep enough into the query.
    auto res = _query->explain();
    if (!res.ok()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(res.errorNumber(), res.errorMessage());
    }
    return res.data;
  }

  void assertNodesMatch(ExecutionPlan const& plan,
                        std::vector<std::string> const& expectedNodes) {
    NodeTypeAsserter asserter{plan, expectedNodes};
    plan.root()->walk(asserter);

    ASSERT_EQ(asserter.numberNodes(), expectedNodes.size())
        << "Unequal number of nodes.\n"
        << "Actual:   " << asserter.numberNodes() << ": " << nodeNames(plan)
        << "\n"
        << "Expected: " << expectedNodes.size() << ": "
        << nodeNames(expectedNodes) << "\n";
  }

  void assertNodesMatch(VPackSlice actualNodes,
                        std::vector<std::string> const& expectedNodes) {
    ASSERT_TRUE(actualNodes.isArray());
    ASSERT_EQ(actualNodes.length(), expectedNodes.size())
        << "Unequal number of nodes.\n"
        << "Actual:   " << actualNodes.length() << ": "
        << nodeNames(actualNodes) << "\n"
        << "Expected: " << expectedNodes.size() << ": "
        << nodeNames(expectedNodes) << "\n";
    for (size_t i = 0; i < expectedNodes.size(); ++i) {
      ASSERT_EQ(actualNodes.at(i).get("type").copyString(), expectedNodes[i])
          << "Unequal node at position #" << i << "\n"
          << "Actual:   " << nodeNames(actualNodes) << "\n"
          << "Expected: " << nodeNames(expectedNodes) << "\n";
    }
  }

  template<class NodeType>
  void matchNodesOfType(ExecutionPlan* plan, ExecutionNode::NodeType type,
                        std::function<void(NodeType const*)> asserter) {
    ::arangodb::containers::SmallVectorWithArena<ExecutionNode*> nodesStorage;
    auto& nodes = nodesStorage.vector();
    plan->findNodesOfType(nodes, type, true);
    for (auto const& n : nodes) {
      NodeType const* casted = ExecutionNode::castTo<NodeType const*>(n);
      ASSERT_NE(casted, nullptr);
      asserter(casted);
    }
  }

  void assertPositionsAreValid(ExecutionPlan* plan) {
#if useOptimize
    BasicPlanRulesAsserter walker;
    plan->root()->walk(walker);
#endif
  }

  std::vector<Variable const*> assertRemoteCollect(
      ExecutionPlan* plan, CollectOptions::CollectMethod method,
      bool isDistinct) {
    bool hasSeenCoordinator = false;
    VarSet coordinatorIn;
    std::vector<Variable const*> coordinatorOut;

    matchNodesOfType<CollectNode>(
        plan, ExecutionNode::NodeType::COLLECT,
        [&](CollectNode const* collect) {
          if (!hasSeenCoordinator) {
#if useOptimize
            EXPECT_TRUE(collect->getPlanSnippet()->isOnCoordinator());
#endif
            // We visit bottom to top, so first we see
            // the coordinator one.
            hasSeenCoordinator = true;
            collect->getVariablesUsedHere(coordinatorIn);
            coordinatorOut = collect->getVariablesSetHere();
            if (method == CollectOptions::CollectMethod::COUNT) {
              EXPECT_EQ(collect->aggregationMethod(),
                        CollectOptions::CollectMethod::SORTED);
            } else {
              EXPECT_EQ(collect->aggregationMethod(), method);
            }
          } else {
#if useOptimize
            EXPECT_FALSE(collect->getPlanSnippet()->isOnCoordinator());
#endif
            for (auto const v : collect->getVariablesSetHere()) {
              // The coordinator needs to collect on all DBServers out Variables
              EXPECT_NE(coordinatorIn.find(v), coordinatorIn.end())
                  << "Did not collect on variable: " << v->name;
            }
            EXPECT_EQ(collect->aggregationMethod(), method);
          }
          EXPECT_EQ(collect->isDistinctCommand(), isDistinct);
          EXPECT_TRUE(collect->isSpecialized());
        });
    return coordinatorOut;
  }

  DistributeQueryRuleTest() {}

  static void SetUpTestCase() {
    server = std::make_unique<mocks::MockCoordinator>();
    // We can register them, but then the API will call count, and the servers
    // do not respond. Now we just get "no endpoint found" but this seems to be
    // okay :shrug: server.registerFakedDBServer("DB1");
    // server.registerFakedDBServer("DB2");
    server->createCollection(server->getSystemDatabase().name(), "collection",
                             {{"s100", "DB1"}, {"s101", "DB2"}},
                             TRI_col_type_e::TRI_COL_TYPE_DOCUMENT);

    // this collection has the same number of shards as "collection", but it is
    // not necessarily sharded in the same way
    server->createCollection(server->getSystemDatabase().name(),
                             "otherCollection",
                             {{"s110", "DB1"}, {"s111", "DB2"}},
                             TRI_col_type_e::TRI_COL_TYPE_DOCUMENT);

    // this collection is sharded like "collection"
    auto optionsVPack = VPackParser::fromJson(
        R"json({"distributeShardsLike": "collection"})json");
    server->createCollection(
        server->getSystemDatabase().name(), "followerCollection",
        {{"s120", "DB1"}, {"s121", "DB2"}},
        TRI_col_type_e::TRI_COL_TYPE_DOCUMENT, optionsVPack->slice());

    // this collection has custom shard keys
    optionsVPack = VPackParser::fromJson(R"json({"shardKeys": ["id"]})json");
    server->createCollection(
        server->getSystemDatabase().name(), "customKeysCollection",
        {{"s123", "DB1"}, {"s234", "DB2"}},
        TRI_col_type_e::TRI_COL_TYPE_DOCUMENT, optionsVPack->slice());
  }

  static void TearDownTestCase() { server.reset(); }

 private:
  std::shared_ptr<Query> _query{nullptr};
};

TEST_F(DistributeQueryRuleTest, single_enumerate_collection) {
  auto queryString = "FOR x IN collection RETURN x";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(*plan, {"SingletonNode", "EnumerateCollectionNode",
                           "RemoteNode", "GatherNode", "ReturnNode"});
}

TEST_F(DistributeQueryRuleTest, no_collection_access) {
  auto queryString = "FOR x IN [1,2,3] RETURN x";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(*plan, {"SingletonNode", "CalculationNode",
                           "EnumerateListNode", "ReturnNode"});
}

TEST_F(DistributeQueryRuleTest, no_collection_access_multiple) {
  auto queryString = "FOR x IN [1,2,3] FOR y IN [1,2,3] RETURN x * y";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(*plan,
                   {"SingletonNode", "CalculationNode", "EnumerateListNode",
                    "EnumerateListNode", "CalculationNode", "ReturnNode"});
}

TEST_F(DistributeQueryRuleTest, document_then_enumerate) {
  auto queryString = R"aql(
    LET doc = DOCUMENT("collection/abc")
      FOR x IN collection
      FILTER x._key == doc.name
      RETURN x)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "ScatterNode", "RemoteNode",
              "EnumerateCollectionNode", "CalculationNode", "FilterNode",
              "RemoteNode", "GatherNode", "ReturnNode"});
}

TEST_F(DistributeQueryRuleTest, many_enumerate_collections) {
  auto queryString = R"aql(
    FOR x IN collection
      FOR y IN collection
      RETURN {x,y})aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan,
      {"SingletonNode", "EnumerateCollectionNode", "RemoteNode", "GatherNode",
       "ScatterNode", "RemoteNode", "EnumerateCollectionNode",
       "CalculationNode", "RemoteNode", "GatherNode", "ReturnNode"});
}

TEST_F(DistributeQueryRuleTest, single_insert) {
  auto queryString = R"aql( INSERT {} INTO collection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "SingleRemoteOperationNode"});
}

TEST_F(DistributeQueryRuleTest, multiple_inserts) {
  auto queryString = R"aql(
    FOR x IN 1..3
    INSERT {} INTO collection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(*plan,
                   {"SingletonNode", "CalculationNode", "CalculationNode",
                    "EnumerateListNode", "CalculationNode", "DistributeNode",
                    "RemoteNode", "InsertNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, enumerate_insert) {
  auto queryString = R"aql(
    FOR x IN collection
    INSERT {} INTO collection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "EnumerateCollectionNode",
              "RemoteNode", "GatherNode", "CalculationNode", "DistributeNode",
              "RemoteNode", "InsertNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, enumerate_update) {
  // Special case here, we enumerate and update the same docs.
  // We could get away without network requests in between
  auto queryString = R"aql(
    FOR x IN collection
    UPDATE x WITH {value: 1} INTO collection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "EnumerateCollectionNode",
              "UpdateNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, enumerate_update_key) {
  // Special case here, we enumerate and update the same docs.
  // We could get away without network requests in between
  auto queryString = R"aql(
    FOR x IN collection
    UPDATE x._key WITH {value: 1} INTO collection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "EnumerateCollectionNode",
              "CalculationNode", "UpdateNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, enumerate_update_custom_shardkey_known) {
  // Special case here, we enumerate and update the same docs.
  // We could get away without network requests in between
  auto queryString = R"aql(
    FOR x IN customKeysCollection
    UPDATE {_key: x._key, id: x.id} WITH {value: 1} INTO customKeysCollection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "EnumerateCollectionNode",
              "CalculationNode", "UpdateNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, enumerate_update_custom_shardkey_unknown) {
  // Special case here, we enumerate and update the same docs.
  // We could get away without network requests in between
  auto queryString = R"aql(
    FOR x IN customKeysCollection
    UPDATE x WITH {value: 1} INTO customKeysCollection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "EnumerateCollectionNode",
              "UpdateNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, enumerate_replace) {
  // Special case here, we enumerate and replace the same docs.
  // We could get away without network requests in between
  auto queryString = R"aql(
    FOR x IN collection
    REPLACE x WITH {value: 1} INTO collection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "EnumerateCollectionNode",
              "ReplaceNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, enumerate_replace_key) {
  // Special case here, we enumerate and replace the same docs.
  // We could get away without network requests in between
  auto queryString = R"aql(
    FOR x IN collection
    REPLACE x._key WITH {value: 1} INTO collection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "EnumerateCollectionNode",
              "CalculationNode", "ReplaceNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, enumerate_replace_custom_shardkey_known) {
  // Special case here, we enumerate and replace the same docs.
  // We could get away without network requests in between
  auto queryString = R"aql(
    FOR x IN customKeysCollection
    REPLACE {_key: x._key, id: x.id} WITH {value: 1} INTO customKeysCollection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "EnumerateCollectionNode",
              "CalculationNode", "ReplaceNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, enumerate_replace_custom_shardkey_unknown) {
  // Special case here, we enumerate and replace the same docs.
  // We could get away without network requests in between
  auto queryString = R"aql(
    FOR x IN customKeysCollection
    REPLACE x WITH {value: 1} INTO customKeysCollection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "EnumerateCollectionNode",
              "ReplaceNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, enumerate_remove_custom_shardkey) {
  // Special case here, we enumerate and remove the same docs.
  // We could get away without network requests in between
  auto queryString = R"aql(
    FOR x IN customKeysCollection
    REMOVE x INTO customKeysCollection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(*plan, {"SingletonNode", "EnumerateCollectionNode",
                           "RemoveNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, distributed_sort) {
  auto queryString = R"aql(
    FOR x IN collection
      SORT x.value DESC
      RETURN x)aql";
  auto plan = optimizeQuery(queryString);

  LOG_DEVEL << nodeNames(*plan);
  assertNodesMatch(
      *plan, {"SingletonNode", "EnumerateCollectionNode", "CalculationNode",
              "SortNode", "RemoteNode", "GatherNode", "ReturnNode"});
  matchNodesOfType<GatherNode>(
      plan.get(), ExecutionNode::NodeType::GATHER,
      [](GatherNode const* gather) {
        EXPECT_EQ(gather->sortMode(), GatherNode::SortMode::MinElement);
        EXPECT_EQ(gather->parallelism(), GatherNode::Parallelism::Parallel);
        auto sortBy = gather->elements();
        // We sort by a temp variable named 1
        ASSERT_EQ(sortBy.size(), 1);
        // TODO We may want to assert the pointer by getting the setter
        EXPECT_EQ(sortBy.at(0).var->name, "1") << sortBy.at(0).toString();
        // We need to keep DESC sort
        EXPECT_FALSE(sortBy.at(0).ascending) << sortBy.at(0).toString();
        // We are not sorting by any attribute here
        EXPECT_TRUE(sortBy.at(0).attributePath.empty())
            << sortBy.at(0).toString();
      });
}

TEST_F(DistributeQueryRuleTest, distributed_collect) {
  auto queryString = R"aql(
    FOR x IN collection
      COLLECT val = x.value
      RETURN val)aql";
  auto plan = optimizeQuery(queryString);

  LOG_DEVEL << nodeNames(*plan);
  assertNodesMatch(
      *plan, {"SingletonNode", "EnumerateCollectionNode", "CalculationNode",
              "CollectNode", "RemoteNode", "GatherNode", "CollectNode",
              "SortNode", "ReturnNode"});
  bool hasSeenCoordinator = false;
  VarSet coordinatorIn;
  std::vector<Variable const*> coordinatorOut;

  // TODO assert collectOptions
  matchNodesOfType<CollectNode>(
      plan.get(), ExecutionNode::NodeType::COLLECT,
      [&](CollectNode const* collect) {
        if (!hasSeenCoordinator) {
          // We visit bottom to top, so first we see
          // the coordinator one.
          hasSeenCoordinator = true;
          collect->getVariablesUsedHere(coordinatorIn);
          coordinatorOut = collect->getVariablesSetHere();
          // We can only have One output variable for this query (val)
          ASSERT_EQ(coordinatorOut.size(), 1);
        } else {
          for (auto const v : collect->getVariablesSetHere()) {
            // The coordinator needs to collect on all DBServers out Variables
            EXPECT_NE(coordinatorIn.find(v), coordinatorIn.end())
                << "Did not collect on variable: " << v->name;
          }
        }
        EXPECT_FALSE(collect->isDistinctCommand());
        EXPECT_TRUE(collect->isSpecialized());
        EXPECT_EQ(collect->aggregationMethod(),
                  CollectOptions::CollectMethod::HASH);
      });
  matchNodesOfType<GatherNode>(
      plan.get(), ExecutionNode::NodeType::GATHER,
      [](GatherNode const* gather) {
        EXPECT_EQ(gather->sortMode(), GatherNode::SortMode::MinElement);
        EXPECT_EQ(gather->parallelism(), GatherNode::Parallelism::Parallel);
        auto sortBy = gather->elements();
        //  Gather is not sorting
        ASSERT_EQ(sortBy.size(), 0);
      });
  matchNodesOfType<SortNode>(plan.get(), ExecutionNode::NodeType::SORT,
                             [&coordinatorOut](SortNode const* sort) {
                               auto sortBy = sort->elements();
                               ASSERT_EQ(sortBy.size(), 1);
                               auto sortElement = sortBy.at(0);
                               EXPECT_EQ(sortElement.var, coordinatorOut.at(0));
                               EXPECT_TRUE(sortElement.ascending);
                               // No attribute path given
                               EXPECT_TRUE(sortElement.attributePath.empty());
                             });
}

TEST_F(DistributeQueryRuleTest, distributed_collect_then_sort) {
  // We first have a collect
  // Then sort on partial output of collect, to make sure the
  // collect could not deliver the ordering in the first place
  // and we require the sort node to exist
  auto queryString = R"aql(
    FOR x IN collection
      COLLECT val = x.value, val2 = x.value2
      SORT val2
      RETURN val)aql";
  auto plan = optimizeQuery(queryString);

  assertNodesMatch(
      *plan, {"SingletonNode", "EnumerateCollectionNode", "CalculationNode",
              "CalculationNode", "CollectNode", "RemoteNode", "GatherNode",
              "CollectNode", "SortNode", "ReturnNode"});

  matchNodesOfType<GatherNode>(
      plan.get(), ExecutionNode::NodeType::GATHER,
      [](GatherNode const* gather) {
        EXPECT_EQ(gather->sortMode(), GatherNode::SortMode::MinElement);
        EXPECT_EQ(gather->parallelism(), GatherNode::Parallelism::Parallel);
        auto sortBy = gather->elements();
        // We do not sort, the sorting is applied after
        // Coordinator sort
        EXPECT_TRUE(sortBy.empty());
      });
  bool hasSeenCoordinator = false;
  VarSet coordinatorIn;
  std::vector<Variable const*> coordinatorOut;
  matchNodesOfType<CollectNode>(
      plan.get(), ExecutionNode::NodeType::COLLECT,
      [&](CollectNode const* collect) {
        if (!hasSeenCoordinator) {
          // We visit bottom to top, so first we see
          // the coordinator one.
          hasSeenCoordinator = true;
          collect->getVariablesUsedHere(coordinatorIn);
          coordinatorOut = collect->getVariablesSetHere();
          // We can only have One output variable for this query (val)
          ASSERT_EQ(coordinatorOut.size(), 2);
        } else {
          for (auto const v : collect->getVariablesSetHere()) {
            // The coordinator needs to collect on all DBServers out Variables
            EXPECT_NE(coordinatorIn.find(v), coordinatorIn.end())
                << "Did not collect on variable: " << v->name;
          }
        }
        EXPECT_FALSE(collect->isDistinctCommand());
        EXPECT_TRUE(collect->isSpecialized());
        EXPECT_EQ(collect->aggregationMethod(),
                  CollectOptions::CollectMethod::HASH);
      });

  matchNodesOfType<SortNode>(plan.get(), ExecutionNode::NodeType::SORT,
                             [&coordinatorOut](SortNode const* sort) {
                               auto sortBy = sort->elements();
                               ASSERT_EQ(sortBy.size(), 1);
                               auto sortElement = sortBy.at(0);
                               EXPECT_EQ(sortElement.var, coordinatorOut.at(1));
                               EXPECT_TRUE(sortElement.ascending);
                               // No attribute path given
                               EXPECT_TRUE(sortElement.attributePath.empty());
                             });
}

TEST_F(DistributeQueryRuleTest, distributed_sort_then_collect) {
  // We first have a sort, then a collect
  // The SORT can be applied on DBServers, then we collect
  // and merge on Coordinator
  auto queryString = R"aql(
    FOR x IN collection
      SORT x.value2
      COLLECT val = x.value
      RETURN val)aql";
  auto plan = optimizeQuery(queryString);

  assertNodesMatch(
      *plan, {"SingletonNode", "EnumerateCollectionNode", "CalculationNode",
              "SortNode", "CalculationNode", "CollectNode", "RemoteNode",
              "GatherNode", "CollectNode", "SortNode", "ReturnNode"});

  matchNodesOfType<GatherNode>(
      plan.get(), ExecutionNode::NodeType::GATHER,
      [](GatherNode const* gather) {
        EXPECT_EQ(gather->sortMode(), GatherNode::SortMode::MinElement);
        EXPECT_EQ(gather->parallelism(), GatherNode::Parallelism::Parallel);
        auto sortBy = gather->elements();
        //  Gather is not sorting
        ASSERT_EQ(sortBy.size(), 0);
      });
  auto coordinatorOut = assertRemoteCollect(
      plan.get(), CollectOptions::CollectMethod::HASH, false);
  // We can only have One output variable for this query (val)
  ASSERT_EQ(coordinatorOut.size(), 1);

  bool hasSeenCoordinator = false;
  matchNodesOfType<SortNode>(
      plan.get(), ExecutionNode::NodeType::SORT,
      [&coordinatorOut, &hasSeenCoordinator](SortNode const* sort) {
        auto sortBy = sort->elements();
        ASSERT_EQ(sortBy.size(), 1);
        auto sortElement = sortBy.at(0);
        EXPECT_TRUE(sortElement.ascending);
        // No attribute path given
        EXPECT_TRUE(sortElement.attributePath.empty());
        if (!hasSeenCoordinator) {
          hasSeenCoordinator = true;
#if useOptimize
          EXPECT_TRUE(sort->getPlanSnippet()->isOnCoordinator());
#endif
          EXPECT_EQ(sortElement.var, coordinatorOut.at(0));
        } else {
#if useOptimize
          EXPECT_FALSE(sort->getPlanSnippet()->isOnCoordinator());
#endif
          // We sort by a different variable
          EXPECT_NE(sortElement.var, coordinatorOut.at(0));
        }
      });
}

TEST_F(DistributeQueryRuleTest, distributed_subquery_dbserver) {
  auto queryString = R"aql(
    FOR y IN 1..3
    LET sub = (
      FOR x IN collection
        FILTER x.value == y
        RETURN x)
     RETURN sub)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "EnumerateListNode",
              "SubqueryStartNode", "ScatterNode", "RemoteNode",
              "EnumerateCollectionNode", "CalculationNode", "FilterNode",
              "RemoteNode", "GatherNode", "SubqueryEndNode", "ReturnNode"});
}

TEST_F(DistributeQueryRuleTest, single_remove) {
  auto queryString = R"aql( REMOVE {_key: "test"} IN collection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(*plan, {"SingletonNode", "SingleRemoteOperationNode"});
}

TEST_F(DistributeQueryRuleTest, distributed_remove) {
  auto queryString = R"aql(
    FOR y IN 1..3
    REMOVE {_key: CONCAT("test", y)} IN collection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "EnumerateListNode",
              "CalculationNode", "CalculationNode", "RemoveNode", "RemoteNode",
              "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, distributed_insert) {
  auto queryString = R"aql(
    FOR y IN 1..3
    INSERT {value: CONCAT("test", y)} IN collection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(*plan,
                   {"SingletonNode", "CalculationNode", "EnumerateListNode",
                    "CalculationNode", "CalculationNode", "DistributeNode",
                    "RemoteNode", "InsertNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, distributed_insert_using_shardkey) {
  auto queryString = R"aql(
    FOR y IN 1..3
    INSERT {_key: CONCAT("test", y)} IN collection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(*plan,
                   {"SingletonNode", "CalculationNode", "EnumerateListNode",
                    "CalculationNode", "CalculationNode", "DistributeNode",
                    "RemoteNode", "InsertNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, distributed_subquery_remove) {
  // NOTE: This test is known to be red right now, it waits for an optimizer
  // rule that can move Calculations out of subqueries.
  auto queryString = R"aql(
    FOR y IN 1..3
    LET sub = (
      REMOVE {_key: CONCAT("test", y)} IN collection
    )
    RETURN sub)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "CalculationNode", "EnumerateListNode",
              "CalculationNode", "CalculationNode", "DistributeNode",
              "RemoteNode", "SubqueryStartNode", "RemoveNode",
              "SubqueryEndNode", "RemoteNode", "GatherNode", "ReturnNode"});
}

TEST_F(DistributeQueryRuleTest, subquery_as_first_node) {
  auto queryString = R"aql(
    LET sub = (
      FOR x IN collection
      RETURN 1
    )
    RETURN LENGTH(sub))aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "SubqueryStartNode", "ScatterNode", "RemoteNode",
              "EnumerateCollectionNode", "RemoteNode", "GatherNode",
              "SubqueryEndNode", "CalculationNode", "ReturnNode"});
}

TEST_F(DistributeQueryRuleTest, enumerate_remove) {
  auto queryString = R"aql(
    FOR doc IN collection
    REMOVE doc IN collection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(*plan, {"SingletonNode", "EnumerateCollectionNode",
                           "RemoveNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, enumerate_remove_key) {
  auto queryString = R"aql(
    FOR doc IN collection
    REMOVE doc._key IN collection)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "EnumerateCollectionNode", "CalculationNode",
              "RemoveNode", "RemoteNode", "GatherNode"});
}

TEST_F(DistributeQueryRuleTest, collect_with_count) {
  auto queryString = R"aql(
    FOR doc IN collection
      COLLECT WITH COUNT INTO c
      RETURN c)aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(*plan,
                   {"SingletonNode", "EnumerateCollectionNode", "CollectNode",
                    "RemoteNode", "GatherNode", "CollectNode", "ReturnNode"});
  assertRemoteCollect(plan.get(), CollectOptions::CollectMethod::COUNT, false);
}

TEST_F(DistributeQueryRuleTest, collect_var_with_count) {
  auto queryString = R"aql(
    FOR doc IN collection
      COLLECT val = doc.val WITH COUNT INTO c
      RETURN {val, c})aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "EnumerateCollectionNode", "CalculationNode",
              "CollectNode", "RemoteNode", "GatherNode", "CollectNode",
              "SortNode", "CalculationNode", "ReturnNode"});
  assertRemoteCollect(plan.get(), CollectOptions::CollectMethod::HASH, false);
}

TEST_F(DistributeQueryRuleTest, collect_with_count_in_subquery) {
  auto queryString = R"aql(
    FOR doc IN collection
      LET sub = (
        FOR d IN collection
          COLLECT val = d.val WITH COUNT INTO c
          RETURN {val, c}
      )
      RETURN {doc, sub})aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(*plan, {"SingletonNode",
                           "SubqueryStartNode",
                           "ScatterNode",
                           "RemoteNode",
                           "EnumerateCollectionNode",
                           "CalculationNode",
                           "CollectNode",
                           "RemoteNode",
                           "GatherNode",
                           "CollectNode",
                           "SortNode",
                           "CalculationNode",
                           "SubqueryEndNode",
                           "ScatterNode",
                           "RemoteNode",
                           "EnumerateCollectionNode",
                           "CalculationNode",
                           "RemoteNode",
                           "GatherNode",
                           "ReturnNode"});
  assertRemoteCollect(plan.get(), CollectOptions::CollectMethod::HASH, false);
}

TEST_F(DistributeQueryRuleTest, return_distinct_subquery) {
  auto queryString = R"aql(
    FOR doc IN collection
      LET sub = (
        FOR d IN collection
          RETURN DISTINCT d.val
      )
      RETURN {doc, sub})aql";
  auto plan = optimizeQuery(queryString);
  assertNodesMatch(
      *plan, {"SingletonNode", "SubqueryStartNode", "ScatterNode", "RemoteNode",
              "EnumerateCollectionNode", "CalculationNode", "CollectNode",
              "RemoteNode", "GatherNode", "CollectNode", "SubqueryEndNode",
              "ScatterNode", "RemoteNode", "EnumerateCollectionNode",
              "CalculationNode", "RemoteNode", "GatherNode", "ReturnNode"});
  assertRemoteCollect(plan.get(), CollectOptions::CollectMethod::DISTINCT,
                      true);
}

}  // namespace aql
}  // namespace tests
}  // namespace arangodb
