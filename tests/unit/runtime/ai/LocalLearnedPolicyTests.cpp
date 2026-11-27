#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AITaskScheduler.h"
#include "Horo/AI/LocalLearnedPolicy.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::AI {
    namespace {
        template <typename Identity> Identity Id(const std::uint64_t value) {
            return Identity::Create(value).Value();
        }

        struct Fixture final {
            LocalPolicyModelId model{Id<LocalPolicyModelId>(1)};
            LocalPolicyTargetId target{Id<LocalPolicyTargetId>(1)};
            std::array<LocalPolicyTargetId, 1> targets{target};
            std::array<LocalPolicyFeatureId, 2> schema{Id<LocalPolicyFeatureId>(10), Id<LocalPolicyFeatureId>(11)};
            TaskId approach{Id<TaskId>(4)};
            TaskId wait{Id<TaskId>(3)};
            std::array<std::int16_t, 2> approachWeights{2, -3};
            std::array<std::int16_t, 2> waitWeights{0, 0};
            std::array<LocalPolicyRowSource, 2> rows{
                LocalPolicyRowSource{approach, 0, approachWeights},
                LocalPolicyRowSource{wait, 0, waitWeights},
            };
            LocalPolicySource source{model, 1, targets, schema, rows};
            LocalPolicyCookTarget cookTarget{target, true, sizeof(LocalLearnedPolicy), 32};
            LocalPolicyRequestIdentity identity{{Id<AiRuntimeIncarnation>(1), {3, 2}}, 2, Id<DecisionGraphAssetId>(9), 3, 4, 5, 10};
            std::array<LocalPolicyFeature, 2> inputs{{{schema[0], 10}, {schema[1], 2}}};

            AiCapabilityDescriptorSet Tasks(const bool includeApproach = true) const {
                AiCapabilityDescriptorRegistry registry;
                REQUIRE(registry.Register({wait}).HasValue());
                if (includeApproach)
                    REQUIRE(registry.Register({approach}).HasValue());
                return registry.Freeze();
            }
        };

        TEST_CASE("Local policy cooker rejects unsupported targets and invalid tensor declarations", "[ai][local-policy]") {
            Fixture fixture;
            const auto tasks = fixture.Tasks();
            const ErrorCodeDescriptor *expected = &AIErrors::TaskContextInvalid;
            SECTION("target has no compatible CPU interpreter") {
                expected = &AIErrors::CapabilityUnavailable;
                fixture.cookTarget.supportsInt16LinearV1 = false;
            }
            SECTION("the selected target is outside the model inventory") {
                expected = &AIErrors::CapabilityUnavailable;
                fixture.cookTarget.id = Id<LocalPolicyTargetId>(2);
            }
            SECTION("the model exceeds target memory") {
                expected = &AIErrors::SchedulerBudgetExhausted;
                fixture.cookTarget.maximumModelBytes = 1;
            }
            SECTION("the model exceeds target work") {
                expected = &AIErrors::SchedulerBudgetExhausted;
                fixture.cookTarget.maximumWorkUnits = 1;
            }
            SECTION("a row does not match the feature tensor shape") {
                fixture.rows.front().weights = {};
            }
            SECTION("the feature identity repeats") {
                fixture.schema[1] = fixture.schema[0];
            }
            SECTION("a row task is not registered") {
                fixture.rows.front().task = Id<TaskId>(192);
            }
            SECTION("tensor schema version is unknown") {
                fixture.source.tensorSchemaVersion = 2;
            }
            const auto invalid = LocalLearnedPolicy::Cook(fixture.source, fixture.cookTarget, tasks);
            REQUIRE(invalid.HasError());
            CHECK(invalid.ErrorValue().code.Value() == expected->code.Value());
        }

        TEST_CASE("Local policy artifacts round trip with exact typed inference", "[ai][local-policy]") {
            const Fixture fixture;
            const auto tasks = fixture.Tasks();
            const auto schema = Id<BlackboardSchemaId>(12);
            const auto cooked = LocalLearnedPolicy::Cook(fixture.source, fixture.cookTarget, tasks, schema);
            REQUIRE(cooked.HasValue());
            const auto bytes = cooked.Value().EncodeCooked();
            REQUIRE(bytes.HasValue());
            const auto digest = ComputeSha256(std::as_bytes(std::span{bytes.Value()}));
            CHECK(digest == cooked.Value().Digest());
            const auto decoded = LocalLearnedPolicy::DecodeCooked(bytes.Value(), digest, fixture.cookTarget, tasks, schema);
            REQUIRE(decoded.HasValue());
            const auto &model = decoded.Value();
            CHECK(model.Digest() == digest);
            const auto encodedAgain = model.EncodeCooked();
            REQUIRE(encodedAgain.HasValue());
            CHECK(encodedAgain.Value() == bytes.Value());
            AiWorkBudget budget{model.RequiredWork()};
            auto proposal = model.Infer(fixture.identity, fixture.target, digest, 1, fixture.inputs, 8, budget);
            LocalPolicyRequestSlot slot{fixture.identity};
            CHECK(slot.Consume(proposal, fixture.identity, model, tasks, 9).task == fixture.approach);
            CHECK(budget.Remaining() == 0);
        }

        TEST_CASE("Local policy artifacts require current target task and byte admission", "[ai][local-policy]") {
            Fixture fixture;
            auto tasks = fixture.Tasks();
            auto schema = Id<BlackboardSchemaId>(12);
            const auto cooked = LocalLearnedPolicy::Cook(fixture.source, fixture.cookTarget, tasks, schema);
            REQUIRE(cooked.HasValue());
            auto artifact = cooked.Value().EncodeCooked();
            REQUIRE(artifact.HasValue());
            auto bytes = std::move(artifact).Value();
            auto digest = cooked.Value().Digest();
            const auto refreshDigest = [&] {
                digest = ComputeSha256(std::as_bytes(std::span{bytes}));
            };
            const ErrorCodeDescriptor *expected = &AIErrors::DecisionAssetSchemaInvalid;
            SECTION("a trailing byte is invalid even with a matching digest") {
                bytes.push_back(0);
                refreshDigest();
            }
            SECTION("truncated weights are invalid even with a matching digest") {
                bytes.pop_back();
                refreshDigest();
            }
            SECTION("an unknown header is invalid") {
                bytes.front() = 0;
                refreshDigest();
            }
            SECTION("corrupted weights fail independent digest verification") {
                bytes.back() ^= 1;
                expected = &AIErrors::DecisionAssetActivationInvalid;
            }
            SECTION("a different Blackboard schema is incompatible") {
                schema = Id<BlackboardSchemaId>(13);
            }
            SECTION("a different target cannot load the artifact") {
                fixture.cookTarget.id = Id<LocalPolicyTargetId>(2);
                expected = &AIErrors::CapabilityUnavailable;
            }
            SECTION("a target with less memory fails admission again") {
                fixture.cookTarget.maximumModelBytes = 1;
                expected = &AIErrors::SchedulerBudgetExhausted;
            }
            SECTION("current task grants may invalidate an otherwise unchanged artifact") {
                tasks = fixture.Tasks(false);
                expected = &AIErrors::TaskContextInvalid;
            }
            const auto outcome = LocalLearnedPolicy::DecodeCooked(bytes, digest, fixture.cookTarget, tasks, schema);
            REQUIRE(outcome.HasError());
            CHECK(outcome.ErrorValue().code.Value() == expected->code.Value());
        }

        TEST_CASE("Local policy inference owns the model and charges its exact integer work", "[ai][local-policy]") {
            Fixture fixture;
            const auto tasks = fixture.Tasks();
            const auto cooked = LocalLearnedPolicy::Cook(fixture.source, fixture.cookTarget, tasks);
            REQUIRE(cooked.HasValue());
            const auto &model = cooked.Value();
            fixture.approachWeights[0] = -20;
            const auto replacement = LocalLearnedPolicy::Cook(fixture.source, fixture.cookTarget, tasks);
            REQUIRE(replacement.HasValue());
            CHECK(replacement.Value().Digest() != model.Digest());
            AiWorkBudget budget{model.RequiredWork()};
            auto proposal = model.Infer(fixture.identity, fixture.target, model.Digest(), 1, fixture.inputs, 8, budget);
            CHECK(proposal.Status() == LocalPolicyStatus::Proposed);
            CHECK(budget.Remaining() == 0);
            LocalPolicyRequestSlot slot{fixture.identity};
            const auto first = slot.Consume(proposal, fixture.identity, model, tasks, 9);
            CHECK(first.status == LocalPolicyStatus::Proposed);
            CHECK(first.task == fixture.approach);
            AiWorkBudget retryBudget{model.RequiredWork()};
            auto retry = model.Infer(fixture.identity, fixture.target, model.Digest(), 1, fixture.inputs, 8, retryBudget);
            CHECK(retry.Status() == LocalPolicyStatus::Proposed);
            const auto repeated = slot.Consume(retry, fixture.identity, model, tasks, 9);
            CHECK(repeated.status == LocalPolicyStatus::Consumed);
            CHECK_FALSE(repeated.task.IsValid());
        }

        TEST_CASE("Local policy inference rejects malformed stale cancelled and over-budget requests", "[ai][local-policy]") {
            Fixture fixture;
            const auto tasks = fixture.Tasks();
            const auto cooked = LocalLearnedPolicy::Cook(fixture.source, fixture.cookTarget, tasks);
            REQUIRE(cooked.HasValue());
            const auto &model = cooked.Value();
            auto expected = LocalPolicyStatus::InvalidInput;
            auto digest = model.Digest();
            auto work = model.RequiredWork();
            std::uint64_t tick = 8;
            CancellationSource cancellation;
            SECTION("tensor input order differs") {
                std::swap(fixture.inputs[0], fixture.inputs[1]);
            }
            SECTION("the expected model digest was replaced") {
                expected = LocalPolicyStatus::Stale;
                digest = {};
            }
            SECTION("the inference work reservation is too small") {
                expected = LocalPolicyStatus::BudgetExhausted;
                --work;
            }
            SECTION("the request expired before worker entry") {
                expected = LocalPolicyStatus::Expired;
                tick = 11;
            }
            SECTION("the owner cancelled inference") {
                expected = LocalPolicyStatus::Cancelled;
                cancellation.RequestCancellation();
            }
            AiWorkBudget budget{work};
            auto proposal = model.Infer(fixture.identity, fixture.target, digest, 1, fixture.inputs, tick, budget, cancellation.Token());
            CHECK(proposal.Status() == expected);
            LocalPolicyRequestSlot slot{fixture.identity};
            const auto failed = slot.Consume(proposal, fixture.identity, model, tasks, 8);
            CHECK_FALSE(failed.task.IsValid());
            CHECK(failed.status == expected);
            AiWorkBudget retryBudget{model.RequiredWork()};
            auto retry = model.Infer(fixture.identity, fixture.target, model.Digest(), 1, fixture.inputs, 8, retryBudget);
            CHECK(slot.Consume(retry, fixture.identity, model, tasks, 8).status == LocalPolicyStatus::Consumed);
        }

        TEST_CASE("Local policy application retires stale proposals without exposing a task", "[ai][local-policy]") {
            Fixture fixture;
            const auto tasks = fixture.Tasks();
            const auto cooked = LocalLearnedPolicy::Cook(fixture.source, fixture.cookTarget, tasks);
            REQUIRE(cooked.HasValue());
            const auto &model = cooked.Value();
            AiWorkBudget budget{model.RequiredWork()};
            auto proposal = model.Infer(fixture.identity, fixture.target, model.Digest(), 1, fixture.inputs, 8, budget);
            REQUIRE(proposal.Status() == LocalPolicyStatus::Proposed);
            auto current = fixture.identity;
            auto currentTasks = tasks;
            auto expected = LocalPolicyStatus::Stale;
            std::uint64_t tick = 9;
            CancellationSource cancellation;
            SECTION("the owner retained another Scene generation") {
                ++current.sceneGeneration;
            }
            SECTION("the provider generation restarted") {
                ++current.providerGeneration;
            }
            SECTION("a different request owns the decision slot") {
                ++current.requestGeneration;
            }
            SECTION("the application deadline passed") {
                expected = LocalPolicyStatus::Expired;
                tick = 11;
            }
            SECTION("the selected task declaration expired") {
                expected = LocalPolicyStatus::MissingTask;
                currentTasks = fixture.Tasks(false);
            }
            SECTION("the owner cancelled before applying") {
                expected = LocalPolicyStatus::Cancelled;
                cancellation.RequestCancellation();
            }
            auto moved = std::move(proposal);
            LocalPolicyRequestSlot slot{fixture.identity};
            CHECK(slot.Consume(proposal, fixture.identity, model, currentTasks, tick).status == LocalPolicyStatus::Consumed);
            const auto rejected = slot.Consume(moved, current, model, currentTasks, tick, cancellation.Token());
            CHECK(rejected.status == expected);
            CHECK_FALSE(rejected.task.IsValid());
            CHECK(slot.Consume(moved, fixture.identity, model, tasks, 8).status == LocalPolicyStatus::Consumed);
        }

        TEST_CASE("Local policy uses stable task identity for equal integer scores", "[ai][local-policy]") {
            Fixture fixture;
            fixture.approachWeights = fixture.waitWeights = {-32768, -32768};
            fixture.inputs.front().value = fixture.inputs.back().value = -32768;
            const auto tasks = fixture.Tasks();
            const auto cooked = LocalLearnedPolicy::Cook(fixture.source, fixture.cookTarget, tasks);
            REQUIRE(cooked.HasValue());
            const auto &model = cooked.Value();
            AiWorkBudget budget{model.RequiredWork()};
            auto candidate = model.Infer(fixture.identity, fixture.target, model.Digest(), 1, fixture.inputs, 8, budget);
            LocalPolicyRequestSlot slot{fixture.identity};
            CHECK(slot.Consume(candidate, fixture.identity, model, tasks, 8).task == fixture.wait);
        }
    }  // namespace
}  // namespace Horo::AI
