#include "AiTestSupport.h"
#include "Horo/AI/BlackboardInstance.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::AI {
    using namespace TestSupport;

    namespace {
        [[nodiscard]] BlackboardValue Bool(const bool value) {
            return BlackboardValue{BlackboardScalarValue{value}};
        }

        [[nodiscard]] BlackboardValue Integer(const std::int64_t value) {
            return BlackboardValue{BlackboardScalarValue{value}};
        }

        [[nodiscard]] BlackboardKeyDescriptor Key(const std::uint64_t id, const BlackboardValueKind kind,
                                                  std::optional<BlackboardValue> value = Bool(false),
                                                  const BlackboardKeyPresence presence = BlackboardKeyPresence::Required,
                                                  const BlackboardKeyAccess access = BlackboardKeyAccess::ReadWrite) {
            return {.key = MakeIdentity<BlackboardKeyId>(id),
                    .kind = kind,
                    .cardinality = BlackboardValueCardinality::Scalar,
                    .maximumCollectionElements = 1,
                    .presence = presence,
                    .access = access,
                    .defaultValue = std::move(value)};
        }

        [[nodiscard]] std::shared_ptr<const BlackboardSchema> Schema(std::vector<BlackboardKeyDescriptor> keys,
                                                                     const std::uint64_t identity = 20, const std::uint32_t version = 1) {
            auto schema = BlackboardSchema::Capture(
                {MakeIdentity<BlackboardSchemaId>(identity), version, BlackboardUnknownValuePolicy::Reject, keys});
            REQUIRE(schema.HasValue());
            return std::make_shared<const BlackboardSchema>(std::move(schema).Value());
        }

        [[nodiscard]] BlackboardInstanceBinding Binding(const std::shared_ptr<const BlackboardSchema> &schema,
                                                        const std::uint64_t runtime = 7, const std::uint32_t generation = 1,
                                                        const std::uint64_t schemaGeneration = 1) {
            auto incarnation = AiRuntimeIncarnation::Create(runtime);
            REQUIRE(incarnation.HasValue());
            return {{incarnation.Value(), {3, 4}}, schema->Identity(), schema->Version(), schemaGeneration, generation};
        }

        [[nodiscard]] std::unique_ptr<BlackboardInstance> Instance(const std::shared_ptr<const BlackboardSchema> &schema) {
            auto instance = BlackboardInstance::Create(Binding(schema), schema);
            REQUIRE(instance.HasValue());
            return std::move(instance).Value();
        }

        [[nodiscard]] TaskHandle Task(const std::uint64_t runtime = 7, const std::uint32_t slot = 8, const std::uint32_t generation = 1) {
            auto incarnation = AiRuntimeIncarnation::Create(runtime);
            REQUIRE(incarnation.HasValue());
            return {incarnation.Value(), {slot, generation}};
        }

        struct ObserverProbe final {
            BlackboardInstance *instance{};
            BlackboardObserverRegistration registration{};
            BlackboardObserverToken token{};
            std::optional<BlackboardWriteBatch> pendingBatch;
            std::array<BlackboardKeyId, MaximumBlackboardKeys> changes{};
            std::size_t changedCount{};
            std::uint64_t revision{};
            std::size_t calls{};
            bool beginRejected{};
            bool commitRejected{};
            bool resetRejected{};
            bool registrationRejected{};
            bool removalRejected{};
            bool cancellationRejected{};
            bool replacementRejected{};
            bool teardownRejected{};
        };

        void Observe(void *context, const BlackboardNotificationBatch &notification) noexcept {
            auto &probe = *static_cast<ObserverProbe *>(context);
            ++probe.calls;
            probe.revision = notification.Revision();
            probe.changedCount = notification.Changes().size();
            std::ranges::copy(notification.Changes(), probe.changes.begin());
            if (probe.instance == nullptr)
                return;
            const auto isReentrant = [](const auto &result) {
                return result.HasError() && result.ErrorValue().code.Value() == AIErrors::BlackboardReentrantMutation.code.Value();
            };
            probe.beginRejected = isReentrant(probe.instance->BeginWriteBatch());
            if (probe.pendingBatch.has_value())
                probe.commitRejected = isReentrant(probe.instance->CommitAtBlackboardSync(std::move(*probe.pendingBatch)));
            probe.resetRejected = isReentrant(probe.instance->ResetAtBlackboardSync());
            probe.registrationRejected = isReentrant(probe.instance->RegisterObserverAtBlackboardSync(probe.registration));
            probe.removalRejected = isReentrant(probe.instance->RemoveObserverAtBlackboardSync(probe.token));
            probe.cancellationRejected = isReentrant(probe.instance->CancelTaskObserversAtBlackboardSync(probe.registration.task));
            probe.replacementRejected = isReentrant(probe.instance->ReplaceAtBlackboardSync({}, {}));
            probe.teardownRejected = isReentrant(probe.instance->TeardownAtBlackboardSync());
        }

        /** @brief Verifies every terminal commit consumes the candidate and prevents later staging. */
        void CheckConsumed(BlackboardWriteBatch &batch) {
            CHECK_FALSE(batch.Binding().IsValid());
            CHECK(batch.BaseRevision() == 0);
            CHECK(batch.Writes().empty());
            ExpectError(batch.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(true)}), AIErrors::BlackboardBatchInvalid);
        }

        /** @brief Stages and commits true values for the requested test keys. */
        void CommitTrueValues(BlackboardInstance &instance, std::initializer_list<std::uint64_t> keyIds) {
            auto batch = instance.BeginWriteBatch().Value();
            for (const auto keyId : keyIds)
                REQUIRE(batch.Stage({MakeIdentity<BlackboardKeyId>(keyId), Bool(true)}).HasValue());
            REQUIRE(instance.CommitAtBlackboardSync(std::move(batch)).HasValue());
        }
    }  // namespace

    TEST_CASE("Blackboard instance materializes typed schema defaults", "[unit][ai][blackboard-instance]") {
        auto schema = Schema({Key(2, BlackboardValueKind::Boolean, Bool(true)), Key(1, BlackboardValueKind::SignedInteger, Integer(9))});
        auto instance = Instance(schema);
        auto snapshot = instance->Snapshot().Value();
        CHECK(snapshot.Revision().Value() == 1);
        CHECK(snapshot.Read(MakeIdentity<BlackboardKeyId>(1)).Value() == std::optional{Integer(9)});
        CHECK(snapshot.Read(MakeIdentity<BlackboardKeyId>(2)).Value() == std::optional{Bool(true)});
        ExpectError(snapshot.Read(MakeIdentity<BlackboardKeyId>(99)), AIErrors::BlackboardUnknownValueRejected);
        static_assert(!std::is_default_constructible_v<BlackboardInstance>);
        static_assert(!std::is_copy_constructible_v<BlackboardWriteBatch>);
        static_assert(std::is_same_v<decltype(std::declval<const BlackboardWriteBatch>().Writes()), std::span<const BlackboardWrite>>);
        static_assert(std::is_same_v<decltype(std::declval<const BlackboardSnapshot>().Read(BlackboardKeyId{})),
                                     Result<std::optional<BlackboardValue>>>);
        static_assert(!std::is_default_constructible_v<BlackboardNotificationBatch>);
        static_assert(!std::is_aggregate_v<BlackboardNotificationBatch>);
        static_assert(std::is_same_v<BlackboardObserverCallback, void (*)(void *, const BlackboardNotificationBatch &) noexcept>);
        static_assert(
            std::is_same_v<decltype(std::declval<const BlackboardNotificationBatch>().Changes()), std::span<const BlackboardKeyId>>);
        static_assert(sizeof(BlackboardNotificationBatch) <= 2048,
                      "Callback-scoped blackboard notifications must stay within the owner-thread stack budget");
        static_assert(sizeof(Result<BlackboardWriteBatch>) <= 4096, "Bounded batch storage must not consume the Windows caller stack");
        static_assert(sizeof(Result<BlackboardSnapshot>) <= 4096, "Immutable snapshot storage must not consume the Windows caller stack");
    }

    TEST_CASE("Blackboard instance rejects missing required defaults and invalid binding", "[unit][ai][blackboard-instance]") {
        auto missing = Schema({Key(1, BlackboardValueKind::Boolean, std::nullopt)});
        ExpectError(BlackboardInstance::Create(Binding(missing), missing), AIErrors::BlackboardInstanceInvalid);
        auto valid = Schema({Key(1, BlackboardValueKind::Boolean)});
        auto binding = Binding(valid);
        binding.schemaGeneration = 0;
        ExpectError(BlackboardInstance::Create(binding, valid), AIErrors::BlackboardInstanceInvalid);
    }

    TEST_CASE("Blackboard batch commits atomically in deterministic key order", "[unit][ai][blackboard-instance]") {
        auto schema =
            Schema({Key(3, BlackboardValueKind::Boolean), Key(1, BlackboardValueKind::Boolean), Key(2, BlackboardValueKind::Boolean)});
        auto instance = Instance(schema);
        auto batch = instance->BeginWriteBatch().Value();
        REQUIRE(batch.Stage({MakeIdentity<BlackboardKeyId>(3), Bool(true)}).HasValue());
        REQUIRE(batch.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(true)}).HasValue());
        auto committed = instance->CommitAtBlackboardSync(std::move(batch));
        REQUIRE(committed.HasValue());
        CheckConsumed(batch);
        CHECK(committed.Value().revision == 2);
        CHECK(std::ranges::equal(committed.Value().Changes(),
                                 std::array{MakeIdentity<BlackboardKeyId>(1), MakeIdentity<BlackboardKeyId>(3)}));
    }

    TEST_CASE("Blackboard invalid write rolls back complete batch", "[unit][ai][blackboard-instance]") {
        auto schema = Schema({Key(1, BlackboardValueKind::Boolean), Key(2, BlackboardValueKind::SignedInteger, Integer(0))});
        auto instance = Instance(schema);
        auto before = instance->Snapshot().Value();
        auto batch = instance->BeginWriteBatch().Value();
        REQUIRE(batch.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(true)}).HasValue());
        auto authorization = std::make_shared<BlackboardValueAuthorization>();
        std::weak_ptr<const BlackboardValueAuthorization> retained = authorization;
        REQUIRE(batch.Stage({MakeIdentity<BlackboardKeyId>(2), Bool(true), authorization}).HasValue());
        authorization.reset();
        ExpectError(instance->CommitAtBlackboardSync(std::move(batch)), AIErrors::BlackboardValueTypeMismatch);
        CHECK(retained.expired());
        CheckConsumed(batch);
        CHECK(before.Revision().Value() == 1);
        CHECK(before.Read(MakeIdentity<BlackboardKeyId>(1)).Value() == std::optional{Bool(false)});
    }

    TEST_CASE("Blackboard batches coalesce duplicate writes and reject unknown and stale writes", "[unit][ai][blackboard-instance]") {
        auto schema = Schema({Key(1, BlackboardValueKind::Boolean)});
        auto instance = Instance(schema);
        auto duplicate = instance->BeginWriteBatch().Value();
        REQUIRE(duplicate.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(true)}).HasValue());
        REQUIRE(duplicate.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(false)}).HasValue());
        REQUIRE(duplicate.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(true)}).HasValue());
        CHECK(duplicate.Writes().size() == 1);
        auto duplicateCommit = instance->CommitAtBlackboardSync(std::move(duplicate));
        REQUIRE(duplicateCommit.HasValue());
        CHECK(duplicateCommit.Value().revision == 2);
        CHECK(instance->Snapshot().Value().Read(MakeIdentity<BlackboardKeyId>(1)).Value() == std::optional{Bool(true)});
        auto unknown = instance->BeginWriteBatch().Value();
        REQUIRE(unknown.Stage({MakeIdentity<BlackboardKeyId>(99), Bool(true)}).HasValue());
        ExpectError(instance->CommitAtBlackboardSync(std::move(unknown)), AIErrors::BlackboardUnknownValueRejected);
        auto stale = instance->BeginWriteBatch().Value();
        auto winner = instance->BeginWriteBatch().Value();
        REQUIRE(winner.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(true)}).HasValue());
        REQUIRE(winner.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(false)}).HasValue());
        REQUIRE(instance->CommitAtBlackboardSync(std::move(winner)).HasValue());
        ExpectError(instance->CommitAtBlackboardSync(std::move(stale)), AIErrors::BlackboardInstanceStale);
        CheckConsumed(stale);
    }

    TEST_CASE("Rejected stale Blackboard consumption releases authorization before caller batch destruction",
              "[unit][ai][blackboard-instance]") {
        auto instance = Instance(Schema({Key(1, BlackboardValueKind::Boolean)}));
        auto stale = instance->BeginWriteBatch().Value();
        auto authorization = std::make_shared<BlackboardValueAuthorization>();
        std::weak_ptr<const BlackboardValueAuthorization> retained = authorization;
        REQUIRE(stale.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(false), authorization}).HasValue());
        authorization.reset();
        REQUIRE_FALSE(retained.expired());
        CommitTrueValues(*instance, {1});
        ExpectError(instance->CommitAtBlackboardSync(std::move(stale)), AIErrors::BlackboardInstanceStale);
        // stale remains in this scope: consumption, not its eventual destructor, must release the lease.
        CHECK(retained.expired());
        CheckConsumed(stale);
        CHECK(instance->Snapshot().Value().Read(MakeIdentity<BlackboardKeyId>(1)).Value() == std::optional{Bool(true)});
    }

    TEST_CASE("Blackboard batch enforces exact and one-over write capacity", "[unit][ai][blackboard-instance]") {
        std::vector<BlackboardKeyDescriptor> keys;
        for (std::uint64_t id = 1; id <= MaximumBlackboardKeys; ++id)
            keys.push_back(Key(id, BlackboardValueKind::Boolean));
        auto instance = Instance(Schema(std::move(keys)));
        auto batch = instance->BeginWriteBatch().Value();
        for (std::uint64_t id = 1; id <= MaximumBlackboardWritesPerBatch; ++id)
            REQUIRE(batch.Stage({MakeIdentity<BlackboardKeyId>(id), Bool(true)}).HasValue());
        REQUIRE(batch.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(false)}).HasValue());
        CHECK(batch.Writes().size() == MaximumBlackboardWritesPerBatch);
        ExpectError(batch.Stage({MakeIdentity<BlackboardKeyId>(MaximumBlackboardWritesPerBatch + 1), Bool(true)}),
                    AIErrors::BlackboardLimitExceeded);
        REQUIRE(instance->CommitAtBlackboardSync(std::move(batch)).HasValue());
    }

    TEST_CASE("Blackboard no-op and changed commits have monotonic revisions", "[unit][ai][blackboard-instance]") {
        auto schema = Schema({Key(1, BlackboardValueKind::Boolean)});
        auto instance = Instance(schema);
        auto noOp = instance->BeginWriteBatch().Value();
        REQUIRE(noOp.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(false)}).HasValue());
        auto unchanged = instance->CommitAtBlackboardSync(std::move(noOp)).Value();
        CHECK(unchanged.revision == 1);
        CHECK(unchanged.Changes().empty());
        auto changed = instance->BeginWriteBatch().Value();
        REQUIRE(changed.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(true)}).HasValue());
        CHECK(instance->CommitAtBlackboardSync(std::move(changed)).Value().revision == 2);
    }

    TEST_CASE("Blackboard snapshots are immutable value copies across ordinary commits", "[unit][ai][blackboard-instance]") {
        auto schema = Schema({Key(1, BlackboardValueKind::Boolean)});
        auto instance = Instance(schema);
        auto before = instance->Snapshot().Value();
        auto changed = instance->BeginWriteBatch().Value();
        REQUIRE(changed.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(true)}).HasValue());
        REQUIRE(instance->CommitAtBlackboardSync(std::move(changed)).HasValue());
        CHECK(before.Revision().Value() == 1);
        CHECK(before.Read(MakeIdentity<BlackboardKeyId>(1)).Value() == std::optional{Bool(false)});
        CHECK(instance->Snapshot().Value().Read(MakeIdentity<BlackboardKeyId>(1)).Value() == std::optional{Bool(true)});
    }

    TEST_CASE("Blackboard commits reject writes to read-only schema keys", "[unit][ai][blackboard-instance]") {
        auto schema =
            Schema({Key(1, BlackboardValueKind::Boolean, Bool(false), BlackboardKeyPresence::Required, BlackboardKeyAccess::ReadOnly)});
        auto instance = Instance(schema);
        auto batch = instance->BeginWriteBatch().Value();
        REQUIRE(batch.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(true)}).HasValue());
        ExpectError(instance->CommitAtBlackboardSync(std::move(batch)), AIErrors::BlackboardBatchInvalid);
        CHECK(instance->Snapshot().Value().Revision().Value() == 1);
    }

    TEST_CASE("Blackboard replacement expires old snapshots and preserves compatible values", "[unit][ai][blackboard-instance]") {
        auto schema = Schema({Key(1, BlackboardValueKind::Boolean)});
        auto instance = Instance(schema);
        CommitTrueValues(*instance, {1});
        auto old = instance->Snapshot().Value();
        auto replacement = Schema({Key(1, BlackboardValueKind::Boolean), Key(2, BlackboardValueKind::Boolean)}, 20, 2);
        auto replacementBinding = Binding(replacement, 7, 2, 2);
        REQUIRE(instance->ReplaceAtBlackboardSync(replacementBinding, replacement).HasValue());
        ExpectError(old.Revision(), AIErrors::BlackboardInstanceStale);
        auto current = instance->Snapshot().Value();
        CHECK(current.Binding() == replacementBinding);
        CHECK(current.Read(MakeIdentity<BlackboardKeyId>(1)).Value() == std::optional{Bool(true)});
        CHECK(current.Revision().Value() == 3);
    }

    TEST_CASE("Blackboard replacement rejects batches from the previous generation", "[unit][ai][blackboard-instance]") {
        auto schema = Schema({Key(1, BlackboardValueKind::Boolean)});
        auto instance = Instance(schema);
        auto stale = instance->BeginWriteBatch().Value();
        auto replacement = Schema({Key(1, BlackboardValueKind::Boolean)}, 20, 2);
        REQUIRE(instance->ReplaceAtBlackboardSync(Binding(replacement, 7, 2, 2), replacement).HasValue());
        ExpectError(instance->CommitAtBlackboardSync(std::move(stale)), AIErrors::BlackboardInstanceStale);
    }

    TEST_CASE("Blackboard failed replacement preserves active instance", "[unit][ai][blackboard-instance]") {
        auto schema = Schema({Key(1, BlackboardValueKind::Boolean)});
        auto instance = Instance(schema);
        auto old = instance->Snapshot().Value();
        auto incompatible = Schema({Key(1, BlackboardValueKind::SignedInteger, Integer(0))}, 20, 2);
        ExpectError(instance->ReplaceAtBlackboardSync(Binding(incompatible, 7, 2, 2), incompatible), AIErrors::BlackboardValueTypeMismatch);
        CHECK(old.Revision().Value() == 1);
        CHECK(instance->Snapshot().Value().Binding().instanceGeneration == 1);

        auto foreignBinding = Binding(schema, 8, 2, 2);
        ExpectError(instance->ReplaceAtBlackboardSync(foreignBinding, schema), AIErrors::BlackboardInstanceInvalid);
        auto sameSchemaGeneration = Binding(schema, 7, 2, 1);
        ExpectError(instance->ReplaceAtBlackboardSync(sameSchemaGeneration, schema), AIErrors::BlackboardInstanceInvalid);
        CHECK(old.Revision().Value() == 1);
    }

    TEST_CASE("Blackboard reset and teardown are explicit lifecycle paths", "[unit][ai][blackboard-instance]") {
        auto schema = Schema(
            {Key(1, BlackboardValueKind::Boolean), Key(2, BlackboardValueKind::Boolean, std::nullopt, BlackboardKeyPresence::Optional)});
        auto instance = Instance(schema);
        CommitTrueValues(*instance, {1, 2});
        auto reset = instance->ResetAtBlackboardSync().Value();
        CHECK(reset.revision == 3);
        CHECK(std::ranges::equal(reset.Changes(), std::array{MakeIdentity<BlackboardKeyId>(1), MakeIdentity<BlackboardKeyId>(2)}));
        CHECK(instance->Snapshot().Value().Read(MakeIdentity<BlackboardKeyId>(2)).Value() == std::nullopt);
        auto snapshot = instance->Snapshot().Value();
        REQUIRE(instance->TeardownAtBlackboardSync().HasValue());
        REQUIRE(instance->TeardownAtBlackboardSync().HasValue());
        CHECK_FALSE(instance->IsActive());
        ExpectError(snapshot.Read(MakeIdentity<BlackboardKeyId>(1)), AIErrors::BlackboardInstanceStale);
        ExpectError(instance->BeginWriteBatch(), AIErrors::BlackboardInstanceStale);
    }

    TEST_CASE("Blackboard reset is a no-op when values already match defaults", "[unit][ai][blackboard-instance]") {
        auto instance = Instance(Schema({Key(1, BlackboardValueKind::Boolean)}));
        auto reset = instance->ResetAtBlackboardSync();
        REQUIRE(reset.HasValue());
        CHECK(reset.Value().revision == 1);
        CHECK(reset.Value().Changes().empty());
        CHECK(instance->Snapshot().Value().Revision().Value() == 1);
    }

    TEST_CASE("Blackboard observers receive one key-sorted revisioned batch at commit", "[unit][ai][blackboard-observer]") {
        auto schema =
            Schema({Key(3, BlackboardValueKind::Boolean), Key(1, BlackboardValueKind::Boolean), Key(2, BlackboardValueKind::Boolean)});
        auto instance = Instance(schema);
        std::array<ObserverProbe, 3> probes{};
        for (std::size_t index = 0; index < probes.size(); ++index) {
            const auto key = MakeIdentity<BlackboardKeyId>(index + 1);
            const BlackboardObserverRegistration registration{instance->Snapshot().Value().Binding().agent, Task(), key, Observe,
                                                              &probes[index]};
            REQUIRE(instance->RegisterObserverAtBlackboardSync(registration).HasValue());
        }
        auto batch = instance->BeginWriteBatch().Value();
        REQUIRE(batch.Stage({MakeIdentity<BlackboardKeyId>(2), Bool(true)}).HasValue());
        REQUIRE(batch.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(true)}).HasValue());
        REQUIRE(instance->CommitAtBlackboardSync(std::move(batch)).HasValue());
        for (std::size_t index = 0; index < 2; ++index) {
            CHECK(probes[index].calls == 1);
            CHECK(probes[index].revision == 2);
            CHECK(std::ranges::equal(std::span{probes[index].changes}.first(probes[index].changedCount),
                                     std::array{MakeIdentity<BlackboardKeyId>(1), MakeIdentity<BlackboardKeyId>(2)}));
        }
        CHECK(probes[2].calls == 0);

        auto noOp = instance->BeginWriteBatch().Value();
        REQUIRE(noOp.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(true)}).HasValue());
        REQUIRE(instance->CommitAtBlackboardSync(std::move(noOp)).HasValue());
        CHECK(probes[0].calls == 1);
    }

    TEST_CASE("Blackboard observer removal and task cancellation prevent later callbacks", "[unit][ai][blackboard-observer]") {
        auto schema =
            Schema({Key(1, BlackboardValueKind::Boolean), Key(2, BlackboardValueKind::Boolean), Key(3, BlackboardValueKind::Boolean)});
        auto instance = Instance(schema);
        const auto agent = instance->Snapshot().Value().Binding().agent;
        const auto firstTask = Task();
        const auto secondTask = Task(7, 9);
        std::array<ObserverProbe, 3> probes{};
        const auto first =
            instance->RegisterObserverAtBlackboardSync({agent, firstTask, MakeIdentity<BlackboardKeyId>(1), Observe, &probes[0]});
        const auto cancelled =
            instance->RegisterObserverAtBlackboardSync({agent, firstTask, MakeIdentity<BlackboardKeyId>(2), Observe, &probes[1]});
        REQUIRE(first.HasValue());
        REQUIRE(cancelled.HasValue());
        REQUIRE(instance->RegisterObserverAtBlackboardSync({agent, secondTask, MakeIdentity<BlackboardKeyId>(3), Observe, &probes[2]})
                    .HasValue());
        CHECK(instance->RemoveObserverAtBlackboardSync(first.Value()).Value());
        CHECK_FALSE(instance->RemoveObserverAtBlackboardSync(first.Value()).Value());
        const auto reused =
            instance->RegisterObserverAtBlackboardSync({agent, secondTask, MakeIdentity<BlackboardKeyId>(1), Observe, &probes[0]});
        REQUIRE(reused.HasValue());
        CHECK(reused.Value().slot == first.Value().slot);
        CHECK(reused.Value().generation != first.Value().generation);
        CHECK(instance->CancelTaskObserversAtBlackboardSync(firstTask).Value() == 1);
        CHECK(instance->CancelTaskObserversAtBlackboardSync(firstTask).Value() == 0);
        CommitTrueValues(*instance, {1, 2, 3});
        CHECK(probes[0].calls == 1);
        CHECK(probes[1].calls == 0);
        CHECK(probes[2].calls == 1);
    }

    TEST_CASE("Blackboard observer registration is validated and strictly bounded", "[unit][ai][blackboard-observer]") {
        auto schema = Schema({Key(1, BlackboardValueKind::Boolean)});
        auto instance = Instance(schema);
        const auto agent = instance->Snapshot().Value().Binding().agent;
        std::array<ObserverProbe, MaximumBlackboardObservers + 1> probes{};
        for (std::size_t index = 0; index < MaximumBlackboardObservers; ++index)
            REQUIRE(instance->RegisterObserverAtBlackboardSync({agent, Task(), MakeIdentity<BlackboardKeyId>(1), Observe, &probes[index]})
                        .HasValue());
        ExpectError(instance->RegisterObserverAtBlackboardSync({agent, Task(), MakeIdentity<BlackboardKeyId>(1), Observe, &probes.back()}),
                    AIErrors::BlackboardObserverLimitExceeded);
        ExpectError(instance->RegisterObserverAtBlackboardSync({agent, Task(), MakeIdentity<BlackboardKeyId>(99), Observe, &probes.back()}),
                    AIErrors::BlackboardObserverInvalid);
        auto foreignAgent = agent;
        foreignAgent.incarnation = AiRuntimeIncarnation::Create(99).Value();
        ExpectError(instance->RegisterObserverAtBlackboardSync(
                        {foreignAgent, Task(), MakeIdentity<BlackboardKeyId>(1), Observe, &probes.back()}),
                    AIErrors::BlackboardObserverInvalid);
        ExpectError(instance->RegisterObserverAtBlackboardSync(
                        {agent, Task(99), MakeIdentity<BlackboardKeyId>(1), Observe, &probes.back()}),
                    AIErrors::BlackboardObserverInvalid);
        ExpectError(instance->RegisterObserverAtBlackboardSync({agent, Task(), MakeIdentity<BlackboardKeyId>(1), nullptr, &probes.back()}),
                    AIErrors::BlackboardObserverInvalid);
    }

    TEST_CASE("Blackboard publication rejects every reentrant mutation entry point", "[unit][ai][blackboard-observer]") {
        auto schema = Schema({Key(1, BlackboardValueKind::Boolean)});
        auto instance = Instance(schema);
        const auto agent = instance->Snapshot().Value().Binding().agent;
        ObserverProbe probe{};
        probe.instance = instance.get();
        probe.registration = {agent, Task(), MakeIdentity<BlackboardKeyId>(1), Observe, &probe};
        auto token = instance->RegisterObserverAtBlackboardSync(probe.registration);
        REQUIRE(token.HasValue());
        probe.token = token.Value();
        probe.pendingBatch.emplace(instance->BeginWriteBatch().Value());
        REQUIRE(probe.pendingBatch->Stage({MakeIdentity<BlackboardKeyId>(1), Bool(true)}).HasValue());
        auto trigger = instance->BeginWriteBatch().Value();
        REQUIRE(trigger.Stage({MakeIdentity<BlackboardKeyId>(1), Bool(true)}).HasValue());
        REQUIRE(instance->CommitAtBlackboardSync(std::move(trigger)).HasValue());
        CHECK(probe.calls == 1);
        CHECK(probe.beginRejected);
        CHECK(probe.commitRejected);
        REQUIRE(probe.pendingBatch.has_value());
        CheckConsumed(*probe.pendingBatch);
        CHECK(probe.resetRejected);
        CHECK(probe.registrationRejected);
        CHECK(probe.removalRejected);
        CHECK(probe.cancellationRejected);
        CHECK(probe.replacementRejected);
        CHECK(probe.teardownRejected);
        CHECK(instance->IsActive());
    }

    TEST_CASE("Blackboard replacement invalidates observers and teardown releases their borrowed contexts",
              "[unit][ai][blackboard-observer]") {
        auto schema = Schema({Key(1, BlackboardValueKind::Boolean)});
        auto instance = Instance(schema);
        ObserverProbe probe{};
        const auto token = instance->RegisterObserverAtBlackboardSync(
            {instance->Snapshot().Value().Binding().agent, Task(), MakeIdentity<BlackboardKeyId>(1), Observe, &probe});
        REQUIRE(token.HasValue());
        auto replacement = Schema({Key(1, BlackboardValueKind::Boolean)}, 20, 2);
        REQUIRE(instance->ReplaceAtBlackboardSync(Binding(replacement, 7, 2, 2), replacement).HasValue());
        ExpectError(instance->RemoveObserverAtBlackboardSync(token.Value()), AIErrors::BlackboardObserverInvalid);
        CommitTrueValues(*instance, {1});
        CHECK(probe.calls == 0);
        REQUIRE(instance->TeardownAtBlackboardSync().HasValue());
    }

    TEST_CASE("Blackboard reset publishes one revisioned observer batch only when defaults change", "[unit][ai][blackboard-observer]") {
        auto schema = Schema({Key(1, BlackboardValueKind::Boolean)});
        auto instance = Instance(schema);
        CommitTrueValues(*instance, {1});
        ObserverProbe probe{};
        REQUIRE(instance
                    ->RegisterObserverAtBlackboardSync(
                        {instance->Snapshot().Value().Binding().agent, Task(), MakeIdentity<BlackboardKeyId>(1), Observe, &probe})
                    .HasValue());
        auto reset = instance->ResetAtBlackboardSync();
        REQUIRE(reset.HasValue());
        CHECK(probe.calls == 1);
        CHECK(probe.revision == reset.Value().revision);
        CHECK(probe.changedCount == 1);
        REQUIRE(instance->ResetAtBlackboardSync().HasValue());
        CHECK(probe.calls == 1);
    }
}  // namespace Horo::AI
