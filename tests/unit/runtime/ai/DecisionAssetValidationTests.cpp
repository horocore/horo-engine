#include "AiTestSupport.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/DecisionAssetValidation.h"
#include "Horo/AI/UtilitySelector.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::AI {
    namespace {
        using TestSupport::MakeIdentity;

        static_assert(!std::is_default_constructible_v<DecisionAssetPlan::ConstructionToken>);
        static_assert(!std::is_default_constructible_v<DecisionAssetPlan::ConstructionData>);

        [[nodiscard]] SourceLocation At(const std::uint32_t line) {
            return SourceLocation{"assets/ai/guard.horo_bt", line, 2};
        }

        [[nodiscard]] BlackboardValue Scalar(const BlackboardScalarValue &value) {
            return BlackboardValue{value};
        }

        [[nodiscard]] BlackboardKeyDescriptor Key(const std::uint64_t identity, const BlackboardValueKind kind,
                                                  std::optional<BlackboardValue> defaultValue,
                                                  const BlackboardKeyAccess access = BlackboardKeyAccess::ReadWrite,
                                                  const BlackboardKeyPresence presence = BlackboardKeyPresence::Required) {
            return {.key = MakeIdentity<BlackboardKeyId>(identity),
                    .kind = kind,
                    .cardinality = BlackboardValueCardinality::Scalar,
                    .maximumCollectionElements = 1,
                    .presence = presence,
                    .access = access,
                    .defaultValue = std::move(defaultValue)};
        }

        [[nodiscard]] std::shared_ptr<const BlackboardSchema> Schema(const std::uint64_t identity, const std::uint32_t version,
                                                                     std::vector<BlackboardKeyDescriptor> keys) {
            auto captured = BlackboardSchema::Capture({.identity = MakeIdentity<BlackboardSchemaId>(identity),
                                                       .version = version,
                                                       .unknownValuePolicy = BlackboardUnknownValuePolicy::Reject,
                                                       .keys = keys});
            REQUIRE(captured.HasValue());
            return std::make_shared<const BlackboardSchema>(std::move(captured).Value());
        }

        [[nodiscard]] DecisionBlackboardRequirement Requirement(const std::uint64_t key, const BlackboardValueKind kind,
                                                                const BlackboardKeyAccess access = BlackboardKeyAccess::ReadOnly,
                                                                const BlackboardKeyPresence presence = BlackboardKeyPresence::Optional,
                                                                const bool requireDefault = false, const std::uint32_t line = 10) {
            return {.key = MakeIdentity<BlackboardKeyId>(key),
                    .kind = kind,
                    .cardinality = BlackboardValueCardinality::Scalar,
                    .access = access,
                    .presence = presence,
                    .requireDefault = requireDefault,
                    .source = At(line)};
        }

        [[nodiscard]] DecisionNodeDescriptor Descriptor(const std::uint64_t type, const std::uint64_t provider,
                                                        const DecisionDescriptorSourceKind sourceKind,
                                                        std::vector<DecisionBlackboardRequirement> requirements = {},
                                                        const std::uint32_t version = 1) {
            return {.type = MakeIdentity<DecisionNodeTypeId>(type),
                    .origin = {sourceKind, MakeIdentity<DecisionProviderId>(provider), version},
                    .requirements = std::move(requirements),
                    .source = At(3)};
        }

        [[nodiscard]] DecisionAssetNode Node(const std::uint64_t identity, const std::uint64_t type,
                                             std::vector<DecisionBlackboardRequirement> requirements = {},
                                             const DecisionVersionRange descriptorVersion = {1, std::numeric_limits<std::uint32_t>::max()},
                                             const std::uint32_t line = 5) {
            return {.id = MakeIdentity<DecisionNodeId>(identity),
                    .type = MakeIdentity<DecisionNodeTypeId>(type),
                    .descriptorVersion = descriptorVersion,
                    .requirements = std::move(requirements),
                    .source = At(line)};
        }

        [[nodiscard]] DecisionSubtreeReference Subtree(const std::uint64_t asset,
                                                       const DecisionPlanKind kind = DecisionPlanKind::BehaviorTree,
                                                       const DecisionVersionRange version = {1, 1}, const std::uint32_t line = 20) {
            return {.asset = MakeIdentity<DecisionGraphAssetId>(asset),
                    .expectedKind = kind,
                    .requiredAssetVersion = version,
                    .source = At(line)};
        }

        [[nodiscard]] DecisionAssetDescriptor Asset(const std::uint64_t identity, const std::uint64_t schema,
                                                    std::vector<DecisionAssetNode> nodes,
                                                    std::vector<DecisionSubtreeReference> subtrees = {},
                                                    const DecisionPlanKind kind = DecisionPlanKind::BehaviorTree,
                                                    const std::uint32_t schemaVersion = CurrentDecisionAssetSchemaVersion) {
            return {.asset = MakeIdentity<DecisionGraphAssetId>(identity),
                    .kind = kind,
                    .schemaVersion = schemaVersion,
                    .blackboardSchema = MakeIdentity<BlackboardSchemaId>(schema),
                    .requiredBlackboardSchemaVersion = {1, std::numeric_limits<std::uint32_t>::max()},
                    .nodes = std::move(nodes),
                    .subtrees = std::move(subtrees),
                    .source = At(1)};
        }

        [[nodiscard]] auto MakeBooleanSchemas() {
            return std::array{Schema(7, 1, {Key(10, BlackboardValueKind::Boolean, Scalar(BlackboardScalarValue{false}))})};
        }

        TEST_CASE("Utility picks a finite stable tie and exposes bounded score evidence", "[unit][ai][utility]") {
            const auto key = MakeIdentity<BlackboardKeyId>(10);
            const auto schema = Schema(7, 1, {Key(10, BlackboardValueKind::Scalar, Scalar(BlackboardScalarValue{0.5}))});
            const AgentHandle agent{AiRuntimeIncarnation::Create(7).Value(), {3, 4}};
            auto blackboard = BlackboardInstance::Create({agent, schema->Identity(), schema->Version(), 1, 1}, schema);
            REQUIRE(blackboard.HasValue());
            auto snapshot = blackboard.Value()->Snapshot();
            REQUIRE(snapshot.HasValue());
            const std::array options{UtilityOption{MakeIdentity<DecisionNodeId>(2), MakeIdentity<TaskId>(7), 1, 0, {{.key = key}}},
                                     UtilityOption{MakeIdentity<DecisionNodeId>(1), MakeIdentity<TaskId>(8), 1, 0, {{.key = key}}}};
            const auto plan = UtilitySelector::Capture(schema, options);
            REQUIRE(plan.HasValue());
            std::array<UtilityScoreRecord, 3> evidence;
            AiWorkBudget shortAgent{3}, global{8};
            const auto yielded = plan.Value().Evaluate(snapshot.Value(), agent, shortAgent, global, evidence);
            REQUIRE(yielded.HasValue());
            CHECK(yielded.Value().budgetExhausted);
            CHECK_FALSE(yielded.Value().selection);
            CHECK(shortAgent.Remaining() == 3);
            CHECK(global.Remaining() == 8);
            AiWorkBudget sufficient{4};
            const auto evaluated = plan.Value().Evaluate(snapshot.Value(), agent, sufficient, global, evidence);
            REQUIRE(evaluated.HasValue());
            REQUIRE(evaluated.Value().selection);
            CHECK(evaluated.Value().selection->action == MakeIdentity<TaskId>(8));
            CHECK(evaluated.Value().selection->score == 0.5);
            CHECK(evaluated.Value().evidenceWritten == 3);
            CHECK(evaluated.Value().evidenceDropped == 1);
            CHECK(evidence[0].option == MakeIdentity<DecisionNodeId>(1));
            CHECK(evidence[0].key == key);
            CHECK_FALSE(evidence[1].key.IsValid());
            CHECK(sufficient.Remaining() == 0);
            CHECK(global.Remaining() == 4);
        }

        TEST_CASE("Utility rejects malformed policies and skips unavailable inputs", "[unit][ai][utility]") {
            const auto key = MakeIdentity<BlackboardKeyId>(10);
            const auto schema = Schema(7, 1,
                                       {Key(10, BlackboardValueKind::Scalar, std::nullopt, BlackboardKeyAccess::ReadOnly,
                                            BlackboardKeyPresence::Optional)});
            UtilityOption option{MakeIdentity<DecisionNodeId>(1), MakeIdentity<TaskId>(2), 1, 0.5, {{.key = key}}};
            option.considerations.front().maximum = std::numeric_limits<double>::infinity();
            CHECK(UtilitySelector::Capture(schema, std::span{&option, 1}).HasError());
            option.considerations.front().maximum = 1;
            option.priority = std::numeric_limits<double>::quiet_NaN();
            CHECK(UtilitySelector::Capture(schema, std::span{&option, 1}).HasError());
            option.priority = 1;
            const auto plan = UtilitySelector::Capture(schema, std::span{&option, 1});
            REQUIRE(plan.HasValue());
            const AgentHandle agent{AiRuntimeIncarnation::Create(7).Value(), {3, 4}};
            auto blackboard = BlackboardInstance::Create({agent, schema->Identity(), schema->Version(), 1, 1}, schema);
            REQUIRE(blackboard.HasValue());
            auto snapshot = blackboard.Value()->Snapshot();
            REQUIRE(snapshot.HasValue());
            AiWorkBudget shared{2};
            const auto evaluated = plan.Value().Evaluate(snapshot.Value(), agent, shared, shared);
            REQUIRE(evaluated.HasValue());
            CHECK_FALSE(evaluated.Value().selection);
            CHECK_FALSE(evaluated.Value().budgetExhausted);
            CHECK(shared.Remaining() == 0);
        }

        TEST_CASE("Utility applies authored curves before weight and multiplier compensation", "[unit][ai][utility]") {
            const auto key = MakeIdentity<BlackboardKeyId>(10);
            const auto schema = Schema(7, 1, {Key(10, BlackboardValueKind::Scalar, Scalar(BlackboardScalarValue{0.5}))});
            const AgentHandle agent{AiRuntimeIncarnation::Create(7).Value(), {3, 4}};
            auto blackboard = BlackboardInstance::Create({agent, schema->Identity(), schema->Version(), 1, 1}, schema);
            REQUIRE(blackboard.HasValue());
            const auto snapshot = blackboard.Value()->Snapshot();
            REQUIRE(snapshot.HasValue());
            UtilityOption option{MakeIdentity<DecisionNodeId>(1), MakeIdentity<TaskId>(2), 1, 0, {{.key = key}}};
            double expected{};
            SECTION("weighted power") {
                option.considerations[0].curve = UtilityResponseCurve::Power;
                option.considerations[0].parameter = 2;
                option.considerations[0].weight = 0.5;
                expected = 0.625;
            }
            SECTION("normalized logistic") {
                option.considerations[0].curve = UtilityResponseCurve::Logistic;
                option.considerations[0].parameter = 8;
                expected = 0.5;
            }
            SECTION("compensated multiplier") {
                option.compensation = 1;
                option.considerations.push_back({.key = key});
                expected = 0.390625;
            }
            const auto plan = UtilitySelector::Capture(schema, std::span{&option, 1});
            REQUIRE(plan.HasValue());
            AiWorkBudget budget{3};
            const auto evaluated = plan.Value().Evaluate(snapshot.Value(), agent, budget, budget);
            REQUIRE(evaluated.HasValue());
            REQUIRE(evaluated.Value().selection);
            CHECK(std::abs(evaluated.Value().selection->score - expected) < 1e-12);
        }

        [[nodiscard]] DecisionNodeDescriptor MakeNativeDescriptor() {
            return Descriptor(101, 501, DecisionDescriptorSourceKind::Native);
        }

        [[nodiscard]] DecisionNodeDescriptor MakeReadOnlyBooleanDescriptor() {
            return Descriptor(101, 501, DecisionDescriptorSourceKind::Native,
                              {Requirement(10, BlackboardValueKind::Boolean, BlackboardKeyAccess::ReadOnly)});
        }

        [[nodiscard]] bool HasCode(const DecisionAssetValidationReport &report, const ErrorCodeDescriptor &descriptor) {
            return std::ranges::any_of(report.Diagnostics(), [&descriptor](const auto &diagnostic) {
                return diagnostic.code.Value() == descriptor.code.Value();
            });
        }

        [[nodiscard]] bool RejectsInvalidRequirement(DecisionBlackboardRequirement requirement) {
            const auto schemas = MakeBooleanSchemas();
            auto descriptor = MakeNativeDescriptor();
            descriptor.requirements.push_back(std::move(requirement));
            const auto result = DecisionAssetCompiler::Compile(Asset(100, 7, {Node(1, 101)}), {}, std::array{descriptor}, schemas);
            return result.HasValue() && !result.Value().IsValid() &&
                   HasCode(result.Value().validation, AIErrors::DecisionAssetSchemaInvalid);
        }

        TEST_CASE("Decision asset compilation resolves typed keys by stable identity and captures defaults", "[unit][ai][decision_asset]") {
            const auto schema = Schema(7, 1,
                                       {Key(20, BlackboardValueKind::Scalar, Scalar(BlackboardScalarValue{2.5})),
                                        Key(10, BlackboardValueKind::SignedInteger, Scalar(BlackboardScalarValue{std::int64_t{42}}))});
            const std::array schemas{schema};
            const std::array descriptors{
                Descriptor(101, 501, DecisionDescriptorSourceKind::Native,
                           {Requirement(10, BlackboardValueKind::SignedInteger, BlackboardKeyAccess::ReadWrite,
                                        BlackboardKeyPresence::Required, true)}),
                Descriptor(102, 502, DecisionDescriptorSourceKind::Package, {Requirement(20, BlackboardValueKind::Scalar)}),
            };
            auto asset = Asset(100, 7, {Node(2, 102), Node(1, 101)});

            const auto compiled = DecisionAssetCompiler::Compile(asset, {}, descriptors, schemas);
            REQUIRE(compiled.HasValue());
            REQUIRE(compiled.Value().IsValid());
            const auto plan = compiled.Value().plan;
            REQUIRE(plan != nullptr);
            CHECK(plan->Asset().Value() == 100);
            CHECK(plan->BlackboardSchema() == schema);
            REQUIRE(plan->Nodes().size() == 2);
            CHECK(plan->Nodes()[0].id.Value() == 1);
            CHECK(plan->Nodes()[1].id.Value() == 2);

            const auto firstBindings = plan->BindingsForNode(MakeIdentity<DecisionNodeId>(1));
            REQUIRE(firstBindings.size() == 1);
            CHECK(firstBindings[0].key.Value() == 10);
            CHECK(firstBindings[0].schemaIndex == 0);
            REQUIRE(firstBindings[0].defaultValue.has_value());
            CHECK(std::get<std::int64_t>(std::get<BlackboardScalarValue>(*firstBindings[0].defaultValue)) == 42);
            CHECK(plan->BindingsForNode(MakeIdentity<DecisionNodeId>(999)).empty());
        }

        TEST_CASE("Decision asset validation reports missing and ambiguous descriptors with source locations",
                  "[unit][ai][decision_asset]") {
            const auto schema = Schema(7, 1, {Key(10, BlackboardValueKind::Boolean, Scalar(BlackboardScalarValue{false}))});
            const std::array schemas{schema};

            auto missingDescriptorAsset = Asset(100, 7, {Node(1, 999)});
            const auto missing = DecisionAssetCompiler::Compile(missingDescriptorAsset, {}, {}, schemas);
            REQUIRE(missing.HasValue());
            CHECK_FALSE(missing.Value().IsValid());
            REQUIRE(HasCode(missing.Value().validation, AIErrors::DecisionAssetDescriptorMissing));
            CHECK(missing.Value().validation.Diagnostics()[0].source.line == 5);

            const auto ambiguousDescriptors = std::array{
                Descriptor(101, 501, DecisionDescriptorSourceKind::Package),
                Descriptor(101, 502, DecisionDescriptorSourceKind::Script),
            };
            auto ambiguousAsset = Asset(101, 7, {Node(1, 101)});
            const auto ambiguous = DecisionAssetCompiler::Compile(ambiguousAsset, {}, ambiguousDescriptors, schemas);
            REQUIRE(ambiguous.HasValue());
            CHECK(HasCode(ambiguous.Value().validation, AIErrors::DecisionAssetDescriptorAmbiguous));

            const std::array ambiguousSchemas{schema, schema};
            const auto ambiguousSchema =
                DecisionAssetCompiler::Compile(Asset(102, 7, {Node(1, 101)}), {},
                                               std::array{Descriptor(101, 501, DecisionDescriptorSourceKind::Native)}, ambiguousSchemas);
            REQUIRE(ambiguousSchema.HasValue());
            CHECK(HasCode(ambiguousSchema.Value().validation, AIErrors::DecisionAssetSchemaAmbiguous));
        }

        TEST_CASE("Decision asset validation rejects typed, access, presence, default, and version mismatches",
                  "[unit][ai][decision_asset]") {
            const auto schema = Schema(7, 1,
                                       {Key(10, BlackboardValueKind::Boolean, std::nullopt, BlackboardKeyAccess::ReadOnly,
                                            BlackboardKeyPresence::Optional)});
            const std::array schemas{schema};
            const std::array descriptors{Descriptor(101, 501, DecisionDescriptorSourceKind::Native,
                                                    {Requirement(10, BlackboardValueKind::SignedInteger, BlackboardKeyAccess::ReadWrite,
                                                                 BlackboardKeyPresence::Required, true)})};
            const auto invalid = DecisionAssetCompiler::Compile(Asset(100, 7, {Node(1, 101)}), {}, descriptors, schemas);
            REQUIRE(invalid.HasValue());
            CHECK(HasCode(invalid.Value().validation, AIErrors::DecisionAssetBindingTypeMismatch));
            CHECK(HasCode(invalid.Value().validation, AIErrors::DecisionAssetBindingAccessMismatch));
            CHECK(HasCode(invalid.Value().validation, AIErrors::DecisionAssetBindingPresenceMismatch));
            CHECK(HasCode(invalid.Value().validation, AIErrors::DecisionAssetBindingDefaultMissing));

            const auto incompatibleSchemaAsset = [&] {
                auto value = Asset(101, 7, {Node(1, 101)});
                value.requiredBlackboardSchemaVersion = {2, 2};
                return value;
            }();
            const auto incompatibleSchema = DecisionAssetCompiler::Compile(incompatibleSchemaAsset, {}, descriptors, schemas);
            REQUIRE(incompatibleSchema.HasValue());
            CHECK(HasCode(incompatibleSchema.Value().validation, AIErrors::DecisionAssetSchemaIncompatible));

            const auto incompatibleDescriptor =
                DecisionAssetCompiler::Compile(Asset(102, 7, {Node(1, 101, {}, {1, 1})}), {},
                                               std::array{Descriptor(101, 501, DecisionDescriptorSourceKind::Script, {}, 2)}, schemas);
            REQUIRE(incompatibleDescriptor.HasValue());
            CHECK(HasCode(incompatibleDescriptor.Value().validation, AIErrors::DecisionAssetDescriptorIncompatible));
        }

        TEST_CASE("Decision asset validation rejects blackboard enum sentinels and unknown values", "[unit][ai][decision_asset]") {
            auto invalidKind = Requirement(10, BlackboardValueKind::Count);
            CHECK(RejectsInvalidRequirement(invalidKind));
            invalidKind.kind = static_cast<BlackboardValueKind>(0xff);
            CHECK(RejectsInvalidRequirement(invalidKind));

            auto invalidCardinality = Requirement(10, BlackboardValueKind::Boolean);
            invalidCardinality.cardinality = BlackboardValueCardinality::Count;
            CHECK(RejectsInvalidRequirement(invalidCardinality));
            invalidCardinality.cardinality = static_cast<BlackboardValueCardinality>(0xff);
            CHECK(RejectsInvalidRequirement(invalidCardinality));

            auto invalidAccess = Requirement(10, BlackboardValueKind::Boolean);
            invalidAccess.access = BlackboardKeyAccess::Count;
            CHECK(RejectsInvalidRequirement(invalidAccess));
            invalidAccess.access = static_cast<BlackboardKeyAccess>(0xff);
            CHECK(RejectsInvalidRequirement(invalidAccess));

            auto invalidPresence = Requirement(10, BlackboardValueKind::Boolean);
            invalidPresence.presence = BlackboardKeyPresence::Count;
            CHECK(RejectsInvalidRequirement(invalidPresence));
            invalidPresence.presence = static_cast<BlackboardKeyPresence>(0xff);
            CHECK(RejectsInvalidRequirement(invalidPresence));
        }

        TEST_CASE("Decision asset validation treats duplicate node identities as schema errors", "[unit][ai][decision_asset]") {
            const auto result = DecisionAssetCompiler::Compile(Asset(100, 7, {Node(1, 101), Node(1, 101)}), {},
                                                               std::array{MakeNativeDescriptor()}, MakeBooleanSchemas());
            REQUIRE(result.HasValue());
            CHECK(HasCode(result.Value().validation, AIErrors::DecisionAssetSchemaInvalid));
            CHECK_FALSE(HasCode(result.Value().validation, AIErrors::DecisionAssetBindingAmbiguous));
        }

        TEST_CASE("Decision asset validation resolves subtrees and rejects missing, ambiguous, incompatible, and cyclic dependencies",
                  "[unit][ai][decision_asset]") {
            const auto schemas = MakeBooleanSchemas();
            const auto descriptors = std::array{MakeNativeDescriptor()};
            const auto grandchild = Asset(300, 7, {Node(3, 101)});
            const auto child = Asset(200, 7, {Node(2, 101)}, {Subtree(300)});
            auto parent = Asset(100, 7, {Node(1, 101)}, {Subtree(200)});

            const auto valid = DecisionAssetCompiler::Compile(parent, std::array{child, grandchild}, descriptors, schemas);
            REQUIRE(valid.HasValue());
            REQUIRE(valid.Value().IsValid());
            REQUIRE(valid.Value().plan->Dependencies().size() == 2);
            CHECK(valid.Value().plan->Dependencies()[0].asset.Value() == 200);
            CHECK(valid.Value().plan->Dependencies()[1].asset.Value() == 300);

            auto assetLimits = DecisionAssetValidationLimits{};
            assetLimits.maximumAssets = 2;
            const auto overAssetLimit =
                DecisionAssetCompiler::Compile(parent, std::array{child, grandchild}, descriptors, schemas, assetLimits);
            REQUIRE(overAssetLimit.HasValue());
            CHECK(HasCode(overAssetLimit.Value().validation, AIErrors::DecisionAssetLimitExceeded));

            const auto missing = DecisionAssetCompiler::Compile(Asset(101, 7, {Node(1, 101)}, {Subtree(999)}), {}, descriptors, schemas);
            REQUIRE(missing.HasValue());
            CHECK(HasCode(missing.Value().validation, AIErrors::DecisionAssetSubtreeMissing));

            const auto incompatibleChild = Asset(201, 7, {Node(2, 101)}, {}, DecisionPlanKind::StateMachine);
            const auto incompatibleParent = Asset(101, 7, {Node(1, 101)}, {Subtree(201)});
            const auto incompatible =
                DecisionAssetCompiler::Compile(incompatibleParent, std::array{incompatibleChild}, descriptors, schemas);
            REQUIRE(incompatible.HasValue());
            CHECK(HasCode(incompatible.Value().validation, AIErrors::DecisionAssetSubtreeIncompatible));

            const auto duplicateChild = DecisionAssetCompiler::Compile(parent, std::array{child, child}, descriptors, schemas);
            REQUIRE(duplicateChild.HasValue());
            CHECK(HasCode(duplicateChild.Value().validation, AIErrors::DecisionAssetSubtreeAmbiguous));

            auto cyclicChild = child;
            cyclicChild.subtrees.push_back(Subtree(100));
            const auto cycle = DecisionAssetCompiler::Compile(parent, std::array{cyclicChild}, descriptors, schemas);
            REQUIRE(cycle.HasValue());
            CHECK(HasCode(cycle.Value().validation, AIErrors::DecisionAssetDependencyCycle));
        }

        TEST_CASE("Decision asset plan store preserves the last valid plan when a schema change is incompatible",
                  "[unit][ai][decision_asset]") {
            const auto schemas = MakeBooleanSchemas();
            const auto descriptors = std::array{MakeReadOnlyBooleanDescriptor()};
            auto validAsset = Asset(100, 7, {Node(1, 101)});

            auto firstCompilation = DecisionAssetCompiler::Compile(validAsset, {}, descriptors, schemas);
            REQUIRE(firstCompilation.HasValue());
            DecisionAssetPlanStore store;
            auto firstActivation = store.TryActivate(std::move(firstCompilation).Value());
            REQUIRE(firstActivation.HasValue());
            REQUIRE(firstActivation.Value().activated);
            const auto lastValid = firstActivation.Value().activePlan;

            validAsset.requiredBlackboardSchemaVersion = {2, 2};
            auto invalidCompilation = DecisionAssetCompiler::Compile(validAsset, {}, descriptors, schemas);
            REQUIRE(invalidCompilation.HasValue());
            auto rejected = store.TryActivate(std::move(invalidCompilation).Value());
            REQUIRE(rejected.HasValue());
            CHECK_FALSE(rejected.Value().activated);
            CHECK(HasCode(rejected.Value().validation, AIErrors::DecisionAssetSchemaIncompatible));
            CHECK(rejected.Value().activePlan == lastValid);
            CHECK(store.ActivePlan() == lastValid);
        }
    }  // namespace
}  // namespace Horo::AI
