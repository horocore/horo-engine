#include "AiTestSupport.h"
#include "Horo/AI/AIIdentity.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        [[nodiscard]] AiRuntimeIncarnation MakeIncarnation(const std::uint64_t value) {
            const auto incarnation = AiRuntimeIncarnation::Create(value);
            REQUIRE(incarnation.HasValue());
            return incarnation.Value();
        }

        template <typename T>
        concept PersistentlySerializableAiIdentity = requires(T value) { SerializeAiIdentity(value); };

        TEST_CASE("Persistent AI identities are strong exact non-zero values", "[unit][ai][identity]") {
            CHECK(AgentId::Create(0).HasError());
            CHECK(ControllerTypeId::Create(0).HasError());
            CHECK(TaskId::Create(0).HasError());
            CHECK(BlackboardSchemaId::Create(0).HasError());
            CHECK(BlackboardKeyId::Create(0).HasError());
            CHECK(SenseTypeId::Create(0).HasError());
            CHECK(StimulusTypeId::Create(0).HasError());
            CHECK(PerceptionListenerTypeId::Create(0).HasError());
            CHECK(PerceptionProviderId::Create(0).HasError());

            CHECK(MakeIdentity<AgentId>(std::numeric_limits<std::uint64_t>::max()).Value() == std::numeric_limits<std::uint64_t>::max());
            static_assert(!std::is_convertible_v<std::uint64_t, AgentId>);
            static_assert(!std::is_same_v<AgentId, ControllerTypeId>);
            static_assert(!std::is_same_v<ControllerTypeId, TaskId>);
            static_assert(!std::is_same_v<BlackboardSchemaId, BlackboardKeyId>);
            static_assert(!std::is_same_v<SenseTypeId, StimulusTypeId>);
            static_assert(!std::is_same_v<PerceptionListenerTypeId, PerceptionProviderId>);
        }

        TEST_CASE("Every persistent AI identity uses canonical fixed-width network byte order", "[unit][ai][identity]") {
            const SerializedAiIdentity expected{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
            const auto check = [&expected]<typename Identity>() {
                const Identity identity = MakeIdentity<Identity>(0x0102030405060708ULL);
                CHECK(SerializeAiIdentity(identity) == expected);
                const auto decoded = DeserializeAiIdentity<typename Identity::IdentityTag>(expected);
                CHECK(decoded.HasValue());
                CHECK(decoded.Value() == identity);
                CHECK(DeserializeAiIdentity<typename Identity::IdentityTag>({}).HasError());
            };
            check.template operator()<AgentId>();
            check.template operator()<ControllerTypeId>();
            check.template operator()<TaskId>();
            check.template operator()<BlackboardSchemaId>();
            check.template operator()<BlackboardKeyId>();
            check.template operator()<SenseTypeId>();
            check.template operator()<StimulusTypeId>();
            check.template operator()<PerceptionListenerTypeId>();
            check.template operator()<PerceptionProviderId>();

            static_assert(PersistentlySerializableAiIdentity<AgentId>);
            static_assert(!PersistentlySerializableAiIdentity<AiRuntimeIncarnation>);
            static_assert(!PersistentlySerializableAiIdentity<AgentHandle>);
        }

        TEST_CASE("AI authored duplication requires a distinct valid identity", "[unit][ai][identity]") {
            const AgentId source = MakeIdentity<AgentId>(41);
            CHECK(ValidateAiIdentityDuplication(source, MakeIdentity<AgentId>(42)).HasValue());
            ExpectError(ValidateAiIdentityDuplication(source, source), AIErrors::DescriptorConflict);
            ExpectError(ValidateAiIdentityDuplication(source, AgentId{}), AIErrors::IdentityInvalid);
        }

        TEST_CASE("AI descriptor identity validation is bounded domain-typed and order-independent", "[unit][ai][identity]") {
            const std::array agents{MakeIdentity<AgentId>(3), MakeIdentity<AgentId>(1), MakeIdentity<AgentId>(2)};
            const std::array controllers{MakeIdentity<ControllerTypeId>(1)};
            const std::array tasks{MakeIdentity<TaskId>(1)};
            const std::array schemas{MakeIdentity<BlackboardSchemaId>(1)};
            const std::array keys{MakeIdentity<BlackboardKeyId>(1)};
            const AiIdentityDescriptorSet valid{agents, controllers, tasks, schemas, keys};
            CHECK(ValidateAiIdentityDescriptorSet(valid).HasValue());

            const std::array duplicateAgents{MakeIdentity<AgentId>(2), MakeIdentity<AgentId>(1), MakeIdentity<AgentId>(2)};
            ExpectError(ValidateAiIdentityDescriptorSet({duplicateAgents, controllers, tasks, schemas, keys}),
                        AIErrors::DescriptorConflict);
            const std::array reorderedDuplicateAgents{MakeIdentity<AgentId>(2), MakeIdentity<AgentId>(2), MakeIdentity<AgentId>(1)};
            ExpectError(ValidateAiIdentityDescriptorSet({reorderedDuplicateAgents, controllers, tasks, schemas, keys}),
                        AIErrors::DescriptorConflict);

            const std::array invalidTasks{TaskId{}};
            ExpectError(ValidateAiIdentityDescriptorSet({agents, controllers, invalidTasks, schemas, keys}), AIErrors::IdentityInvalid);

            std::vector<AgentId> overLimit;
            overLimit.reserve(MaximumAiIdentityDescriptors + 1);
            for (std::size_t index = 0; index <= MaximumAiIdentityDescriptors; ++index)
                overLimit.push_back(MakeIdentity<AgentId>(index + 1));
            ExpectError(ValidateAiIdentityDescriptorSet({.agents = overLimit}), AIErrors::DescriptorLimitExceeded);
        }

        TEST_CASE("AI runtime handles include scene incarnation slot and generation", "[unit][ai][identity]") {
            const AiRuntimeIncarnation scene = MakeIncarnation(7);
            const AgentHandle original{scene, {3, 4}};
            CHECK(original.IsValid());
            CHECK(ValidateAiRuntimeHandle(original, scene).HasValue());
            ExpectError(ValidateAiRuntimeHandle(AgentHandle{}, scene), AIErrors::HandleInvalid);
            ExpectError(ValidateAiRuntimeHandle(original, {}), AIErrors::HandleInvalid);
            ExpectError(ValidateAiRuntimeHandle(original, MakeIncarnation(8)), AIErrors::HandleInvalid);
            CHECK_FALSE(AgentHandle{scene, {decltype(original.slot)::InvalidIndex, 4}}.IsValid());
            CHECK_FALSE(AgentHandle{scene, {3, 0}}.IsValid());
            CHECK(original != AgentHandle{scene, {3, 5}});
            CHECK(original != AgentHandle{MakeIncarnation(8), {3, 4}});
            static_assert(!std::is_same_v<AgentHandle, TaskHandle>);
            static_assert(std::is_trivially_copyable_v<AgentHandle>);
        }

        TEST_CASE("AI runtime generations advance without stale alias or wrap", "[unit][ai][identity]") {
            const auto next = AdvanceAiRuntimeGeneration(1);
            REQUIRE(next.HasValue());
            CHECK(next.Value() == 2);
            CHECK(AgentHandle{MakeIncarnation(9), {2, 1}} != AgentHandle{MakeIncarnation(9), {2, next.Value()}});

            const auto invalid = AdvanceAiRuntimeGeneration(0);
            REQUIRE(invalid.HasError());
            CHECK(invalid.ErrorValue().code.Value() == AIErrors::HandleInvalid.code.Value());
            const auto exhausted = AdvanceAiRuntimeGeneration(std::numeric_limits<std::uint32_t>::max());
            REQUIRE(exhausted.HasError());
            CHECK(exhausted.ErrorValue().code.Value() == AIErrors::GenerationExhausted.code.Value());
        }

        [[nodiscard]] auto MakeCoreAiErrorDescriptors() {
            return std::array{
                &AIErrors::IdentityInvalid,
                &AIErrors::DescriptorConflict,
                &AIErrors::DescriptorLimitExceeded,
                &AIErrors::HandleInvalid,
                &AIErrors::GenerationExhausted,
                &AIErrors::TaskContextInvalid,
                &AIErrors::TaskTransitionInvalid,
                &AIErrors::TaskFailureInvalid,
                &AIErrors::RuntimeUnavailable,
                &AIErrors::TaskCapacityExceeded,
                &AIErrors::BlackboardSchemaInvalid,
                &AIErrors::BlackboardLimitExceeded,
                &AIErrors::BlackboardValueTypeMismatch,
                &AIErrors::BlackboardValueInvalid,
                &AIErrors::BlackboardUnknownValueRejected,
                &AIErrors::BlackboardStorageUnavailable,
                &AIErrors::BlackboardInstanceStale,
                &AIErrors::BlackboardInstanceInvalid,
                &AIErrors::BlackboardBatchInvalid,
                &AIErrors::BlackboardRevisionExhausted,
                &AIErrors::BlackboardObserverInvalid,
                &AIErrors::BlackboardObserverLimitExceeded,
                &AIErrors::BlackboardReentrantMutation,
                &AIErrors::PerceptionDescriptorInvalid,
                &AIErrors::PerceptionDescriptorLimitExceeded,
                &AIErrors::PerceptionDescriptorConflict,
                &AIErrors::PerceptionDependencyMissing,
                &AIErrors::PerceptionDescriptorIncompatible,
                &AIErrors::PerceptionCapabilityUnavailable,
                &AIErrors::PerceptionRegistryStorageUnavailable,
            };
        }

        [[nodiscard]] auto MakeDecisionAssetErrorDescriptors() {
            return std::array{
                &AIErrors::DecisionAssetSchemaInvalid,           &AIErrors::DecisionAssetLimitExceeded,
                &AIErrors::DecisionAssetDescriptorMissing,       &AIErrors::DecisionAssetDescriptorAmbiguous,
                &AIErrors::DecisionAssetDescriptorIncompatible,  &AIErrors::DecisionAssetSchemaMissing,
                &AIErrors::DecisionAssetSchemaAmbiguous,         &AIErrors::DecisionAssetSchemaIncompatible,
                &AIErrors::DecisionAssetBindingMissing,          &AIErrors::DecisionAssetBindingAmbiguous,
                &AIErrors::DecisionAssetBindingTypeMismatch,     &AIErrors::DecisionAssetBindingAccessMismatch,
                &AIErrors::DecisionAssetBindingPresenceMismatch, &AIErrors::DecisionAssetBindingDefaultMissing,
                &AIErrors::DecisionAssetSubtreeMissing,          &AIErrors::DecisionAssetSubtreeAmbiguous,
                &AIErrors::DecisionAssetSubtreeIncompatible,     &AIErrors::DecisionAssetDependencyCycle,
                &AIErrors::DecisionAssetStorageUnavailable,      &AIErrors::DecisionAssetActivationInvalid,
            };
        }

        TEST_CASE("AI identity errors expose stable unique descriptors", "[unit][ai][errors]") {
            std::set<std::string_view> uniqueCodes;
            const auto validate = [&uniqueCodes](const auto &descriptors) {
                for (const ErrorCodeDescriptor *descriptor : descriptors) {
                    CHECK(descriptor->domain.Value() == "horo.ai");
                    CHECK(uniqueCodes.insert(descriptor->code.Value()).second);
                    CHECK_FALSE(descriptor->summary.empty());
                    CHECK_FALSE(descriptor->remediationHint.empty());
                }
            };
            validate(MakeCoreAiErrorDescriptors());
            validate(MakeDecisionAssetErrorDescriptors());
            CHECK(AIErrors::GenerationExhausted.defaultSeverity == ErrorSeverity::Critical);
        }
    }  // namespace
}  // namespace Horo::AI
