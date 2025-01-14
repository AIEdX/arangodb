////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Julia Volmer
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#include <gtest/gtest.h>
#include <unordered_set>
#include "fmt/format.h"
#include "velocypack/SharedSlice.h"
#include "VelocypackUtils/VelocyPackStringLiteral.h"

#include "Actor/ActorPID.h"
#include "Actor/Runtime.h"

#include "Actors/FinishingActor.h"
#include "Actors/SpawnActor.h"
#include "Actors/TrivialActor.h"
#include "Actors/PingPongActors.h"
#include "ThreadPoolScheduler.h"

using namespace arangodb::pregel::actor;
using namespace arangodb::pregel::actor::test;

struct MockScheduler {
  auto start(size_t number_of_threads) -> void{};
  auto stop() -> void{};
  auto operator()(auto fn) { fn(); }
};

struct EmptyExternalDispatcher {
  auto operator()(ActorPID sender, ActorPID receiver,
                  arangodb::velocypack::SharedSlice msg) -> void {}
};

template<typename T>
class ActorRuntimeTest : public testing::Test {
 public:
  ActorRuntimeTest() : scheduler{std::make_shared<T>()} {
    scheduler->start(number_of_threads);
  }

  std::shared_ptr<T> scheduler;
  size_t number_of_threads = 128;
};
using SchedulerTypes = ::testing::Types<MockScheduler, ThreadPoolScheduler>;
TYPED_TEST_SUITE(ActorRuntimeTest, SchedulerTypes);

TYPED_TEST(ActorRuntimeTest, formats_runtime_and_actor_state) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      ServerID{"PRMR-1234"}, "RuntimeTest", this->scheduler, dispatcher);
  auto actorID = runtime->template spawn<pong_actor::Actor>(
      pong_actor::PongState{}, pong_actor::Start{});

  this->scheduler->stop();
  ASSERT_EQ(
      fmt::format("{}", *runtime),
      R"({"myServerID":"PRMR-1234","runtimeID":"RuntimeTest","uniqueActorIDCounter":1,"actors":[{"id":0,"type":"PongActor"}]})");
  auto actor =
      runtime->template getActorStateByID<pong_actor::Actor>(actorID).value();
  ASSERT_EQ(fmt::format("{}", actor), R"({"called":1})");
  runtime->softShutdown();
}

TYPED_TEST(ActorRuntimeTest, serializes_an_actor_including_its_actor_state) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      ServerID{"PRMR-1234"}, "RuntimeTest", this->scheduler, dispatcher);
  auto actor = runtime->template spawn<TrivialActor>(
      TrivialState{.state = "foo"}, TrivialStart());

  this->scheduler->stop();
  using namespace arangodb::velocypack;
  auto expected =
      R"({"pid":{"server":"PRMR-1234","id":0},"state":{"state":"foo","called":1},"batchsize":16})"_vpack;
  ASSERT_EQ(runtime->getSerializedActorByID(actor)->toJson(),
            expected.toJson());
  runtime->softShutdown();
}

TYPED_TEST(ActorRuntimeTest, spawns_actor) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      "PRMR-1234", "RuntimeTest", this->scheduler, dispatcher);

  auto actor = runtime->template spawn<TrivialActor>(
      TrivialState{.state = "foo"}, TrivialStart());

  this->scheduler->stop();
  auto state = runtime->template getActorStateByID<TrivialActor>(actor);
  ASSERT_EQ(state, (TrivialState{.state = "foo", .called = 1}));
  runtime->softShutdown();
}

TYPED_TEST(ActorRuntimeTest, sends_initial_message_when_spawning_actor) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      "PRMR-1234", "RuntimeTest", this->scheduler, dispatcher);

  auto actor = runtime->template spawn<TrivialActor>(
      TrivialState{.state = "foo"}, TrivialMessage("bar"));

  this->scheduler->stop();
  auto state = runtime->template getActorStateByID<TrivialActor>(actor);
  ASSERT_EQ(state, (TrivialState{.state = "foobar", .called = 1}));
  runtime->softShutdown();
}

TYPED_TEST(ActorRuntimeTest, gives_all_existing_actor_ids) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      "PRMR-1234", "RuntimeTest", this->scheduler, dispatcher);

  ASSERT_TRUE(runtime->getActorIDs().empty());

  auto actor_foo = runtime->template spawn<TrivialActor>(
      TrivialState{.state = "foo"}, TrivialStart());
  auto actor_bar = runtime->template spawn<TrivialActor>(
      TrivialState{.state = "bar"}, TrivialStart());

  this->scheduler->stop();
  auto allActorIDs = runtime->getActorIDs();
  ASSERT_EQ(allActorIDs.size(), 2);
  ASSERT_EQ(
      (std::unordered_set<ActorID>(allActorIDs.begin(), allActorIDs.end())),
      (std::unordered_set<ActorID>{actor_foo, actor_bar}));
  runtime->softShutdown();
}

TYPED_TEST(ActorRuntimeTest, sends_message_to_an_actor) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      "PRMR-1234", "RuntimeTest", this->scheduler, dispatcher);
  auto actor = runtime->template spawn<TrivialActor>(
      TrivialState{.state = "foo"}, TrivialStart{});

  runtime->dispatch(ActorPID{.server = "PRMR-1234", .id = actor},
                    ActorPID{.server = "PRMR-1234", .id = actor},
                    TrivialActor::Message{TrivialMessage("baz")});

  this->scheduler->stop();
  auto state = runtime->template getActorStateByID<TrivialActor>(actor);
  ASSERT_EQ(state, (TrivialState{.state = "foobaz", .called = 2}));
  runtime->softShutdown();
}

struct SomeMessage {};
template<typename Inspector>
auto inspect(Inspector& f, SomeMessage& x) {
  return f.object(x).fields();
}
struct SomeMessages : std::variant<SomeMessage> {
  using std::variant<SomeMessage>::variant;
};
template<typename Inspector>
auto inspect(Inspector& f, SomeMessages& x) {
  return f.variant(x).unqualified().alternatives(
      arangodb::inspection::type<SomeMessage>("someMessage"));
}
TYPED_TEST(
    ActorRuntimeTest,
    actor_receiving_wrong_message_type_sends_back_unknown_error_message) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      "PRMR-1234", "RuntimeTest", this->scheduler, dispatcher);
  auto actor_id = runtime->template spawn<TrivialActor>(
      TrivialState{.state = "foo"}, TrivialStart{});
  auto actor = ActorPID{.server = "PRMR-1234", .id = actor_id};

  runtime->dispatch(actor, actor, SomeMessages{SomeMessage{}});

  this->scheduler->stop();
  ASSERT_EQ(
      runtime->template getActorStateByID<TrivialActor>(actor_id),
      (TrivialState{.state = fmt::format("sent unknown message to {}", actor),
                    .called = 2}));
  runtime->softShutdown();
}

TYPED_TEST(
    ActorRuntimeTest,
    actor_receives_actor_not_found_message_after_trying_to_send_message_to_non_existent_actor) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      "PRMR-1234", "RuntimeTest", this->scheduler, dispatcher);
  auto actor_id = runtime->template spawn<TrivialActor>(
      TrivialState{.state = "foo"}, TrivialStart{});
  auto actor = ActorPID{.server = "PRMR-1234", .id = actor_id};

  auto unknown_actor = ActorPID{.server = "PRMR-1234", .id = {999}};
  runtime->dispatch(actor, unknown_actor,
                    TrivialActor::Message{TrivialMessage{"baz"}});

  this->scheduler->stop();
  ASSERT_EQ(runtime->template getActorStateByID<TrivialActor>(actor_id),
            (TrivialState{.state = fmt::format("receiving actor {} not found",
                                               unknown_actor),
                          .called = 2}));
  runtime->softShutdown();
}

TYPED_TEST(ActorRuntimeTest, ping_pong_game) {
  auto serverID = ServerID{"PRMR-1234"};
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      serverID, "RuntimeTest", this->scheduler, dispatcher);

  auto pong_actor = runtime->template spawn<pong_actor::Actor>(
      pong_actor::PongState{}, pong_actor::Start{});
  auto ping_actor = runtime->template spawn<ping_actor::Actor>(
      ping_actor::PingState{},
      ping_actor::Start{.pongActor =
                            ActorPID{.server = serverID, .id = pong_actor}});

  this->scheduler->stop();
  auto ping_actor_state =
      runtime->template getActorStateByID<ping_actor::Actor>(ping_actor);
  ASSERT_EQ(ping_actor_state,
            (ping_actor::PingState{.called = 2, .message = "hello world"}));
  auto pong_actor_state =
      runtime->template getActorStateByID<pong_actor::Actor>(pong_actor);
  ASSERT_EQ(pong_actor_state, (pong_actor::PongState{.called = 2}));
  runtime->softShutdown();
}

TYPED_TEST(ActorRuntimeTest, spawn_game) {
  auto serverID = ServerID{"PRMR-1234"};
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      serverID, "RuntimeTest", this->scheduler, dispatcher);

  auto spawn_actor =
      runtime->template spawn<SpawnActor>(SpawnState{}, SpawnStartMessage{});

  runtime->dispatch(ActorPID{.server = serverID, .id = spawn_actor},
                    ActorPID{.server = serverID, .id = spawn_actor},
                    SpawnActor::Message{SpawnMessage{"baz"}});

  this->scheduler->stop();
  ASSERT_EQ(runtime->getActorIDs().size(), 2);
  ASSERT_EQ(runtime->template getActorStateByID<SpawnActor>(spawn_actor),
            (SpawnState{.called = 2, .state = "baz"}));
  runtime->softShutdown();
}

TYPED_TEST(ActorRuntimeTest, finishes_actor_when_actor_says_so) {
  auto serverID = ServerID{"PRMR-1234"};
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      serverID, "RuntimeTest", this->scheduler, dispatcher);

  auto finishing_actor = runtime->template spawn<FinishingActor>(
      FinishingState{}, FinishingStart{});

  runtime->dispatch(ActorPID{.server = serverID, .id = finishing_actor},
                    ActorPID{.server = serverID, .id = finishing_actor},
                    FinishingActor::Message{FinishingFinish{}});

  this->scheduler->stop();
  ASSERT_TRUE(
      runtime->actors.find(finishing_actor)->get()->isFinishedAndIdle());
  runtime->softShutdown();
}

TYPED_TEST(ActorRuntimeTest, garbage_collects_finished_actor) {
  auto serverID = ServerID{"PRMR-1234"};
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      serverID, "RuntimeTest", this->scheduler, dispatcher);
  auto finishing_actor = runtime->template spawn<FinishingActor>(
      FinishingState{}, FinishingStart{});

  runtime->dispatch(ActorPID{.server = serverID, .id = finishing_actor},
                    ActorPID{.server = serverID, .id = finishing_actor},
                    FinishingActor::Message{FinishingFinish{}});
  // wait for actor to work off all messages
  while (not runtime->areAllActorsIdle()) {
  }

  runtime->garbageCollect();

  this->scheduler->stop();
  ASSERT_EQ(runtime->actors.size(), 0);
  runtime->softShutdown();
}

TYPED_TEST(ActorRuntimeTest, garbage_collects_all_finished_actors) {
  auto serverID = ServerID{"PRMR-1234"};
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      serverID, "RuntimeTest", this->scheduler, dispatcher);

  auto actor_to_be_finished = runtime->template spawn<FinishingActor>(
      FinishingState{}, FinishingStart{});
  runtime->template spawn<FinishingActor>(FinishingState{}, FinishingStart{});
  runtime->template spawn<FinishingActor>(FinishingState{}, FinishingStart{});
  auto another_actor_to_be_finished = runtime->template spawn<FinishingActor>(
      FinishingState{}, FinishingStart{});
  runtime->template spawn<FinishingActor>(FinishingState{}, FinishingStart{});

  runtime->dispatch(ActorPID{.server = serverID, .id = actor_to_be_finished},
                    ActorPID{.server = serverID, .id = actor_to_be_finished},
                    FinishingActor::Message{FinishingFinish{}});
  runtime->dispatch(
      ActorPID{.server = serverID, .id = another_actor_to_be_finished},
      ActorPID{.server = serverID, .id = another_actor_to_be_finished},
      FinishingActor::Message{FinishingFinish{}});
  // wait for actor to work off all messages
  while (not runtime->areAllActorsIdle()) {
  }

  runtime->garbageCollect();

  this->scheduler->stop();
  ASSERT_EQ(runtime->actors.size(), 3);
  auto remaining_actor_ids = runtime->getActorIDs();
  std::unordered_set<ActorID> actor_ids(remaining_actor_ids.begin(),
                                        remaining_actor_ids.end());
  ASSERT_FALSE(actor_ids.contains(actor_to_be_finished));
  ASSERT_FALSE(actor_ids.contains(another_actor_to_be_finished));
  runtime->softShutdown();
}

TYPED_TEST(ActorRuntimeTest,
           finishes_and_garbage_collects_all_actors_when_shutting_down) {
  auto serverID = ServerID{"PRMR-1234"};
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<Runtime<TypeParam, EmptyExternalDispatcher>>(
      serverID, "RuntimeTest", this->scheduler, dispatcher);
  runtime->template spawn<TrivialActor>(TrivialState{}, TrivialStart{});
  runtime->template spawn<TrivialActor>(TrivialState{}, TrivialStart{});
  runtime->template spawn<TrivialActor>(TrivialState{}, TrivialStart{});
  runtime->template spawn<TrivialActor>(TrivialState{}, TrivialStart{});
  runtime->template spawn<TrivialActor>(TrivialState{}, TrivialStart{});
  ASSERT_EQ(runtime->actors.size(), 5);
  // wait for actor to work off all messages
  while (not runtime->areAllActorsIdle()) {
  }
  this->scheduler->stop();
  runtime->softShutdown();
  ASSERT_EQ(runtime->actors.size(), 0);
}

TEST(ActorRuntimeTest, sends_messages_between_lots_of_actors) {
  auto serverID = ServerID{"PRMR-1234"};
  auto scheduler = std::make_shared<ThreadPoolScheduler>();
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime =
      std::make_shared<Runtime<ThreadPoolScheduler, EmptyExternalDispatcher>>(
          serverID, "RuntimeTest", scheduler, dispatcher);
  scheduler->start(128);
  size_t actor_count = 128;

  for (size_t i = 0; i < actor_count; i++) {
    runtime->template spawn<TrivialActor>(TrivialState{}, TrivialStart{});
  }

  // send from actor i to actor i+1 a message with content i
  for (size_t i = 0; i < actor_count; i++) {
    runtime->dispatch(
        ActorPID{.server = serverID, .id = ActorID{(i + 1) % actor_count}},
        ActorPID{.server = serverID, .id = ActorID{i}},
        TrivialActor::Message{TrivialMessage{std::to_string(i)}});
  }

  // wait for actor to work off all messages
  while (not runtime->areAllActorsIdle()) {
  }

  scheduler->stop();
  ASSERT_EQ(runtime->actors.size(), actor_count);
  for (size_t i = 0; i < actor_count; i++) {
    ASSERT_EQ(runtime->template getActorStateByID<TrivialActor>(ActorID{i}),
              (TrivialState{.state = std::to_string(i), .called = 2}));
  }
  runtime->softShutdown();
}
