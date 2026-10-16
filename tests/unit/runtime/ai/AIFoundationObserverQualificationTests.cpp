#include "AISceneRuntimeDetail.h"
#include "AiSceneTestSupport.h"
#include "AllocationProbe.h"

#include <array>
#include <memory>
#include <vector>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        /** @brief Owns production-prepared agent storage and uses the production publication shutdown boundary. */
        struct OwnedPublication final {
            TestSupport::ActivationFixture fixture{TestSupport::MakeFixture(1, 11, 7, 0)};
            Detail::AiSceneRuntimeState state;

            explicit OwnedPublication(std::shared_ptr<const BlackboardSchema> schema = {}, const std::uint64_t incarnation = 7) {
                fixture.binding = {MakeIdentity<AiRuntimeIncarnation>(incarnation), Runtime::SceneRuntimeId{incarnation}};
                fixture.owner.runtime = fixture.binding.scene;
                fixture.agent.owner = fixture.owner;
                if (schema) {
                    fixture.schema = std::move(schema);
                    fixture.plan = TestSupport::MakePlan(fixture.schema, 71);
                    fixture.descriptor.blackboardSchema = fixture.schema;
                    fixture.descriptor.decisionPlan = fixture.plan;
                    fixture.agent.controller->blackboardSchema = fixture.schema->Identity();
                }
                state.binding = fixture.binding;
                const std::array slots{Horo::Handle<AgentHandleTag>{0, 1}};
                auto agents = Detail::PrepareStructuralAgents(fixture.binding, std::array{fixture.agent}, std::array{fixture.descriptor},
                                                              AiSceneRuntimeSettings{}.availableCapabilities, slots);
                REQUIRE(agents.HasValue());
                state.agents = std::move(agents).Value();
                state.agentsByOwner.emplace(fixture.owner, 0);
            }

            ~OwnedPublication() {
                Detail::ShutdownStateContents(state);
            }

            BlackboardInstance &Board() {
                return *state.agents.front().blackboard;
            }

            TaskHandle Start() {
                auto task = Detail::StartOwnedTask(state, state.agents.front().record.handle, MakeIdentity<TaskId>(41));
                REQUIRE(task.HasValue());
                return task.Value();
            }
        };

        struct ObserverCount final {
            std::size_t calls{};
            std::size_t changedKeys{};
        };

        void CountNotification(void *context, const BlackboardNotificationBatch &notification) noexcept {
            auto &counter = *static_cast<ObserverCount *>(context);
            ++counter.calls;
            counter.changedKeys = notification.Changes().size();
        }

        struct BorrowedObserver final {
            std::shared_ptr<ObserverCount> counter;
        };

        void CountBorrowedNotification(void *context, const BlackboardNotificationBatch &notification) noexcept {
            CountNotification(static_cast<BorrowedObserver *>(context)->counter.get(), notification);
        }

        void CommitTrue(BlackboardInstance &board, const BlackboardKeyId key) {
            auto preparedWrites = board.BeginWriteBatch();
            REQUIRE(preparedWrites.HasValue());
            auto writes = std::move(preparedWrites).Value();
            REQUIRE(writes.Stage({key, BlackboardValue{BlackboardScalarValue{true}}}).HasValue());
            REQUIRE(board.CommitAtBlackboardSync(std::move(writes)).HasValue());
        }

        TEST_CASE("AI foundation production publication shutdown releases observers tasks and blackboard storage",
                  "[ai][qualification][owner-internal]") {
            auto count = std::make_shared<ObserverCount>();
            auto context = std::make_shared<BorrowedObserver>(BorrowedObserver{count});
            std::weak_ptr<BorrowedObserver> weakContext = context;
            OwnedPublication owner;
            const auto task = owner.Start();
            const auto agent = owner.state.agents.front().record.handle;
            const auto key = owner.fixture.schema->Keys().front().key;
            const auto cancellation = owner.state.agents.front().task->Context()->cancellation;
            auto token = owner.Board().RegisterObserverAtBlackboardSync({agent, task, key, CountBorrowedNotification, context.get()});
            REQUIRE(token.HasValue());
            CommitTrue(owner.Board(), key);
            REQUIRE(count->calls == 1);
            auto snapshot = owner.Board().Snapshot();
            auto detached = owner.Board().BeginWriteBatch();
            REQUIRE(snapshot.HasValue());
            REQUIRE(detached.HasValue());

            // This is the existing complete-publication owner entry point used by replacement, shutdown and destruction.
            Detail::ShutdownStateContents(owner.state);
            CHECK(owner.state.closed);
            CHECK(owner.state.agents.empty());
            CHECK(owner.state.agentsByOwner.empty());
            CHECK(cancellation.IsCancellationRequested());
            context.reset();
            CHECK(weakContext.expired());
            ExpectError(snapshot.Value().Read(key), AIErrors::BlackboardInstanceStale);
            ExpectError(Detail::StartOwnedTask(owner.state, agent, MakeIdentity<TaskId>(42)), AIErrors::HandleInvalid);

            OwnedPublication replacement{owner.fixture.schema, 8};
            ExpectError(replacement.Board().CommitAtBlackboardSync(std::move(detached).Value()), AIErrors::BlackboardInstanceStale);
            ExpectError(replacement.Board().RemoveObserverAtBlackboardSync(token.Value()), AIErrors::BlackboardObserverInvalid);
            CommitTrue(replacement.Board(), key);
            CHECK(count->calls == 1);
            CHECK(replacement.Board().CaptureCanonical().Value().entries.size() == 1);
        }

        std::vector<BlackboardKeyDescriptor> MaximumKeys() {
            std::vector<BlackboardKeyDescriptor> keys;
            keys.reserve(MaximumBlackboardKeys);
            for (std::size_t index = 0; index < MaximumBlackboardKeys; ++index)
                keys.push_back(
                    {.key = MakeIdentity<BlackboardKeyId>(71 + index), .defaultValue = BlackboardValue{BlackboardScalarValue{false}}});
            return keys;
        }

        /** @brief Borrows caller-owned counters for the full observer limit and verifies overflow is rejected. */
        void RegisterMaximumObservers(OwnedPublication &owner, const TaskHandle task, const BlackboardKeyId key,
                                      std::array<ObserverCount, MaximumBlackboardObservers + 1> &counters) {
            const auto agent = owner.state.agents.front().record.handle;
            for (std::size_t index = 0; index < MaximumBlackboardObservers; ++index)
                REQUIRE(owner.Board().RegisterObserverAtBlackboardSync({agent, task, key, CountNotification, &counters[index]}).HasValue());
            ExpectError(owner.Board().RegisterObserverAtBlackboardSync({agent, task, key, CountNotification, &counters.back()}),
                        AIErrors::BlackboardObserverLimitExceeded);
        }

        /** @brief Prepares maximum writes and repeated updates before the allocation measurement begins. */
        BlackboardWriteBatch PrepareMaximumBatch(BlackboardInstance &board, const std::vector<BlackboardKeyDescriptor> &keys) {
            auto preparedBatch = board.BeginWriteBatch();
            REQUIRE(preparedBatch.HasValue());
            auto batch = std::move(preparedBatch).Value();
            for (const auto &key : keys)
                REQUIRE(batch.Stage({key.key, BlackboardValue{BlackboardScalarValue{true}}}).HasValue());
            for (std::size_t index = 0; index < 4096; ++index)
                REQUIRE(batch.Stage({keys.front().key, BlackboardValue{BlackboardScalarValue{index % 2 != 0}}}).HasValue());
            CHECK(batch.Writes().size() == MaximumBlackboardWritesPerBatch);
            return batch;
        }

        TEST_CASE("AI foundation declared maximum storage remains bounded through notification and commit",
                  "[ai][qualification][capacity]") {
            auto keys = MaximumKeys();
            const BlackboardSchemaDescriptor descriptor{MakeIdentity<BlackboardSchemaId>(70), 1, BlackboardUnknownValuePolicy::Reject,
                                                        keys};
            auto captured = BlackboardSchema::Capture(descriptor);
            REQUIRE(captured.HasValue());
            auto schema = std::make_shared<const BlackboardSchema>(std::move(captured).Value());
            std::array<ObserverCount, MaximumBlackboardObservers + 1> counters{};
            OwnedPublication owner{schema};
            const auto task = owner.Start();
            RegisterMaximumObservers(owner, task, keys.front().key, counters);
            auto batch = PrepareMaximumBatch(owner.Board(), keys);
            std::size_t allocations{};
            auto committed = [&] {
                Tests::AllocationProbe::ScopedMeasurement measurement;
                auto result = owner.Board().CommitAtBlackboardSync(std::move(batch));
                allocations = measurement.Snapshot().requests;
                return result;
            }();
            REQUIRE(committed.HasValue());
            CHECK(allocations == 0);
            CHECK_FALSE(batch.Binding().IsValid());
            CHECK(batch.BaseRevision() == 0);
            CHECK(batch.Writes().empty());
            CHECK(committed.Value().changedKeyCount == MaximumBlackboardKeys);
            CHECK(owner.Board().CaptureCanonical().Value().entries.size() == MaximumBlackboardKeys);
            for (std::size_t index = 0; index < MaximumBlackboardObservers; ++index) {
                CHECK(counters[index].calls == 1);
                CHECK(counters[index].changedKeys == MaximumBlackboardKeys);
            }
            const auto before = owner.Board().CaptureCanonical().Value();
            keys.push_back({.key = MakeIdentity<BlackboardKeyId>(999), .defaultValue = BlackboardValue{BlackboardScalarValue{false}}});
            ExpectError(BlackboardSchema::Capture({descriptor.identity, 1, descriptor.unknownValuePolicy, keys}),
                        AIErrors::BlackboardLimitExceeded);
            CHECK(owner.Board().CaptureCanonical().Value() == before);
        }

        TEST_CASE("AI foundation malformed schema diagnostics preserve the admitted owner publication",
                  "[ai][qualification][diagnostics]") {
            OwnedPublication owner;
            const auto before = owner.Board().CaptureCanonical().Value();
            const auto valid = owner.fixture.schema->Keys().front();
            SECTION("duplicate stable identity") {
                const std::array keys{valid, valid};
                ExpectError(BlackboardSchema::Capture({owner.fixture.schema->Identity(), 1, BlackboardUnknownValuePolicy::Reject, keys}),
                            AIErrors::DescriptorConflict);
            }
            SECTION("invalid key kind") {
                auto invalid = valid;
                invalid.kind = BlackboardValueKind::Count;
                const std::array keys{invalid};
                ExpectError(BlackboardSchema::Capture({owner.fixture.schema->Identity(), 1, BlackboardUnknownValuePolicy::Reject, keys}),
                            AIErrors::BlackboardSchemaInvalid);
            }
            SECTION("wrong typed default") {
                auto invalid = valid;
                invalid.defaultValue = BlackboardValue{BlackboardScalarValue{std::int64_t{9}}};
                const std::array keys{invalid};
                ExpectError(BlackboardSchema::Capture({owner.fixture.schema->Identity(), 1, BlackboardUnknownValuePolicy::Reject, keys}),
                            AIErrors::BlackboardValueTypeMismatch);
            }
            CHECK(owner.Board().CaptureCanonical().Value() == before);
        }

        TEST_CASE("AI foundation inline collections reject over-limit state before replacing owned storage",
                  "[ai][qualification][capacity]") {
            OwnedPublication owner;
            const auto before = owner.Board().CaptureCanonical().Value();
            const BlackboardKeyDescriptor key{.key = MakeIdentity<BlackboardKeyId>(300),
                                              .cardinality = BlackboardValueCardinality::Collection,
                                              .maximumCollectionElements = MaximumBlackboardCollectionElements};
            BlackboardCollectionValue collection;
            collection.count = MaximumBlackboardCollectionElements;
            REQUIRE(ValidateBlackboardValue(BlackboardValue{collection}, &key, BlackboardUnknownValuePolicy::Reject).HasValue());
            ++collection.count;
            ExpectError(ValidateBlackboardValue(BlackboardValue{collection}, &key, BlackboardUnknownValuePolicy::Reject),
                        AIErrors::BlackboardLimitExceeded);
            CHECK(owner.Board().CaptureCanonical().Value() == before);
        }
    }  // namespace
}  // namespace Horo::AI
