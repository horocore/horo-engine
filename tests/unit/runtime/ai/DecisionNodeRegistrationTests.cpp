#include "BehaviorTreeRuntimeTestSupport.h"
#include "Horo/AI/DecisionNodeRegistration.h"
#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/NativeDecisionNode.h"

#include <thread>

namespace Horo::AI {
    namespace {
        using BehaviorTreeTestSupport::Fixture;
        using BehaviorTreeTestSupport::Node;
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        DecisionNodeRegistration Registration(std::shared_ptr<BehaviorTreeTestSupport::Probe> &probe) {
            DecisionNodeRegistration registration;
            registration.contract.descriptor = {.type = MakeIdentity<DecisionNodeTypeId>(101),
                                                .origin = {DecisionDescriptorSourceKind::Native, MakeIdentity<DecisionProviderId>(501), 1},
                                                .requirements = {},
                                                .source = {__FILE__, __LINE__, 1}};
            registration.factory = {&probe, [](void *value, std::span<const BehaviorTreeProperty>) -> IBehaviorTreeExecutor * {
                return new BehaviorTreeTestSupport::Executor(*static_cast<std::shared_ptr<BehaviorTreeTestSupport::Probe> *>(value));
            }, [](void *, IBehaviorTreeExecutor *value) noexcept {
                delete value;
            }};
            return registration;
        }

        TEST_CASE("Decision node declarations preserve unavailable metadata without granting execution", "[ai][decision-node]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            auto registration = Registration(fixture.probe);
            registration.factory = {};
            auto admission = std::make_shared<std::atomic_bool>(true);
            auto captured = DecisionNodeGeneration::Capture({&registration, 1}, admission);
            REQUIRE(captured.HasValue());
            CHECK(captured.Value()->Contracts().size() == 1);
            auto executor = captured.Value()->CreateExecutor(captured.Value(), *fixture.Bindings(), {});
            REQUIRE(executor.HasError());
            CHECK(executor.ErrorValue().code.Value() == AIErrors::DecisionAssetDescriptorMissing.code.Value());
            CHECK(fixture.probe->destroyed == 0);
        }

        TEST_CASE("Decision node generation rejects conflicts malformed metadata and excessive input", "[ai][decision-node]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            const auto valid = Registration(fixture.probe);
            auto admission = std::make_shared<std::atomic_bool>(true);
            const std::array repeated{valid, valid};
            CHECK(DecisionNodeGeneration::Capture(repeated, admission).HasError());
            auto malformed = valid;
            malformed.factory.destroy = nullptr;
            CHECK(ValidateDecisionNodeRegistration(malformed).HasError());
            malformed = valid;
            malformed.contract.capabilities.bits = 0xffffffffU;
            CHECK(ValidateDecisionNodeRegistration(malformed).HasError());
            malformed = valid;
            malformed.contract.descriptor.source = {};
            CHECK(ValidateDecisionNodeRegistration(malformed).HasError());
            malformed = valid;
            malformed.contract.descriptor.requirements = {{.key = MakeIdentity<BlackboardKeyId>(10)}};
            CHECK(ValidateDecisionNodeRegistration(malformed).HasError());
            malformed = valid;
            malformed.contract.ports = {{1}, {1}};
            CHECK(ValidateDecisionNodeRegistration(malformed).HasError());
            malformed.contract.ports.resize(129);
            CHECK(ValidateDecisionNodeRegistration(malformed).HasError());
            std::vector<DecisionNodeRegistration> excessive(DecisionNodeGeneration::MaximumNodes + 1, valid);
            CHECK(DecisionNodeGeneration::Capture(excessive, admission).HasError());
            CHECK(fixture.probe->destroyed == 0);
        }

        TEST_CASE("Decision node factories validate exact provider version and host capability before creation", "[ai][decision-node]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            auto registration = Registration(fixture.probe);
            auto admission = std::make_shared<std::atomic_bool>(true);
            registration.contract.capabilities = AiCapabilitySet::Of(AiCapability::Navigation);
            auto captured = DecisionNodeGeneration::Capture({&registration, 1}, admission);
            REQUIRE(captured.HasValue());
            CHECK(captured.Value()->CreateExecutor(captured.Value(), *fixture.Bindings(), {}).HasError());
            CHECK(captured.Value()->CreateExecutor(captured.Value(), *fixture.Bindings(), registration.contract.capabilities).HasValue());
            CHECK(fixture.probe->destroyed == 1);
            registration.contract.descriptor.origin.version = 2;
            captured = DecisionNodeGeneration::Capture({&registration, 1}, admission);
            REQUIRE(captured.HasValue());
            CHECK(captured.Value()->CreateExecutor(captured.Value(), *fixture.Bindings(), registration.contract.capabilities).HasError());
            CHECK(fixture.probe->destroyed == 1);
        }

        TEST_CASE("Decision node executor retirement aborts and cleans running work once while retaining its generation",
                  "[ai][decision-node]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            fixture.probe->outcome[1] = AiTaskState::Running;
            const auto registration = Registration(fixture.probe);
            auto admission = std::make_shared<std::atomic_bool>(true);
            std::weak_ptr<const std::atomic_bool> weak = admission;
            auto captured = DecisionNodeGeneration::Capture({&registration, 1}, admission);
            REQUIRE(captured.HasValue());
            auto generation = std::move(captured).Value();
            auto executor = generation->CreateExecutor(generation, *fixture.Bindings(), {});
            REQUIRE(executor.HasValue());
            const BehaviorTreeInstanceBinding binding{fixture.agent,
                                                      {},
                                                      100,
                                                      1,
                                                      {fixture.agent, fixture.schema->Identity(), fixture.schema->Version(), 1, 1}};
            auto created = BehaviorTreeInstance::Create(fixture.Plan(), binding, std::move(executor).Value());
            REQUIRE(created.HasValue());
            auto instance = std::move(created).Value();
            CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
            admission->store(false);
            admission.reset();
            CHECK_FALSE(weak.expired());
            CHECK_FALSE(generation->CanAdmit());
            instance->Shutdown();
            instance.reset();
            CHECK(fixture.probe->aborts[1] == 1);
            CHECK(fixture.probe->cleanups[1] == 1);
            CHECK(fixture.probe->destroyed == 1);
            generation.reset();
            CHECK(weak.expired());
        }

        TEST_CASE("Bound decision executor replacement prepares new node callbacks before cancelling the old tree", "[ai][decision-node]") {
            using enum BehaviorTreeOperation;
            using enum AiTaskState;
            Fixture fixture({Node(1, Task)});
            fixture.probe->outcome[1] = Running;
            const auto registration = Registration(fixture.probe);
            auto admission = std::make_shared<std::atomic_bool>(true);
            auto captured = DecisionNodeGeneration::Capture(std::span{&registration, 1}, admission);
            REQUIRE(captured.HasValue());
            auto generation = captured.Value();
            auto original = fixture.Plan();
            auto callbacks = generation->CreateExecutor(generation, *original, {});
            REQUIRE(callbacks.HasValue());
            BehaviorTreeInstanceBinding binding{fixture.agent, {}, 100, 1, fixture.blackboard->Binding()};
            binding.availableTaskSlots = 8;
            auto created = BehaviorTreeInstance::Create(original, binding, std::move(callbacks).Value());
            REQUIRE(created.HasValue());
            auto instance = std::move(created).Value();
            REQUIRE(fixture.Evaluate(*instance, 1) == Running);
            fixture.nodes = {Node(1, Sequence, {2, 3}), Node(2, Task), Node(3, Task)};
            fixture.probe->outcome[3] = Running;
            auto candidate = fixture.Plan();
            ExpectError(instance->Replace(candidate), AIErrors::DecisionAssetDescriptorIncompatible);
            CHECK(fixture.probe->aborts[1] == 0);
            auto replacement = generation->CreateExecutor(generation, *candidate, {});
            REQUIRE(replacement.HasValue());
            auto replaced = instance->Replace(candidate, std::move(replacement).Value());
            REQUIRE(replaced.HasValue());
            CHECK_FALSE(replaced.Value());
            CHECK(fixture.probe->aborts[1] == 1);
            CHECK(fixture.probe->cleanups[1] == 1);
            CHECK(fixture.probe->starts[2] == 0);
            REQUIRE(fixture.Evaluate(*instance, 2) == Running);
            CHECK(fixture.probe->starts[2] == 1);
            CHECK(fixture.probe->starts[3] == 1);
        }

        TEST_CASE("Decision node generation rejects off-owner creation without touching factories", "[ai][decision-node]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            const auto registration = Registration(fixture.probe);
            auto captured = DecisionNodeGeneration::Capture({&registration, 1}, std::make_shared<std::atomic_bool>(true));
            REQUIRE(captured.HasValue());
            const auto plan = fixture.Bindings();
            bool rejected{};
            std::thread worker([&] {
                rejected = captured.Value()->CreateExecutor(captured.Value(), *plan, {}).HasError();
            });
            worker.join();
            CHECK(rejected);
            CHECK(fixture.probe->destroyed == 0);
        }

        TEST_CASE("Decision node authored properties are typed defaulted and checked before factories", "[ai][decision-node]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            auto registration = Registration(fixture.probe);
            registration.contract.properties = {{MakeIdentity<BehaviorTreePropertyId>(4), "weight", BehaviorTreePropertyKind::Scalar, 1.0}};
            auto captured = DecisionNodeGeneration::Capture({&registration, 1}, std::make_shared<std::atomic_bool>(true));
            REQUIRE(captured.HasValue());
            auto wrong = registration.contract.properties;
            wrong[0].value = std::numeric_limits<double>::infinity();
            const std::array configuration{DecisionNodeConfiguration{MakeIdentity<DecisionNodeId>(1), wrong}};
            CHECK(captured.Value()->CreateExecutor(captured.Value(), *fixture.Bindings(), {}, configuration).HasError());
            CHECK(fixture.probe->destroyed == 0);
            CHECK(captured.Value()->CreateExecutor(captured.Value(), *fixture.Bindings(), {}).HasValue());
            CHECK(fixture.probe->destroyed == 1);
        }

        TEST_CASE("Generated node bundle rejects the old ABI before accepting its new node records", "[ai][decision-node][abi]") {
            Gameplay::GeneratedGameplayDescriptorBundle bundle;
            bundle.moduleId = "game.node.tests";
            bundle.buildFingerprint = "test";
            bundle.descriptorRevision = 1;
            bundle.lifecycle = {[]() noexcept -> Gameplay::IGameModule * {
                return nullptr;
            }, [](Gameplay::IGameModule *) noexcept {
            }};
            const Gameplay::GameModuleLoadExpectation expected{"game.node.tests", "test", 1};
            REQUIRE(Gameplay::ValidateGeneratedGameplayDescriptorBundle(bundle, expected).HasValue());
            bundle.sdkBoundaryVersion = 22;
            CHECK(Gameplay::ValidateGeneratedGameplayDescriptorBundle(bundle, expected).HasError());
            bundle.sdkBoundaryVersion = Gameplay::GameplaySdkBoundaryVersion;
            bundle.decisionNodeCount = 1;
            CHECK(Gameplay::ValidateGeneratedGameplayDescriptorBundle(bundle, expected).HasError());
        }

        TEST_CASE("Executable node binding instantiates provider leaves while core controls remain flat records", "[ai][decision-node]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Sequence, {2}), Node(2, BehaviorTreeOperation::Task)});
            const auto registration = Registration(fixture.probe);
            auto captured = DecisionNodeGeneration::Capture({&registration, 1}, std::make_shared<std::atomic_bool>(true));
            REQUIRE(captured.HasValue());
            const auto plan = fixture.Plan();
            auto executor = captured.Value()->CreateExecutor(captured.Value(), *plan, {});
            REQUIRE(executor.HasValue());
            const BehaviorTreeInstanceBinding binding{fixture.agent,
                                                      {},
                                                      100,
                                                      1,
                                                      {fixture.agent, fixture.schema->Identity(), fixture.schema->Version(), 1, 1}};
            auto created = BehaviorTreeInstance::Create(plan, binding, std::move(executor).Value());
            REQUIRE(created.HasValue());
            auto instance = std::move(created).Value();
            CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Succeeded);
            CHECK(fixture.probe->starts[1] == 0);
            CHECK(fixture.probe->starts[2] == 1);
            instance.reset();
            CHECK(fixture.probe->destroyed == 1);
        }

        TEST_CASE("Composed node generations preserve canonical ordering ambiguity rejection and each real revocation",
                  "[ai][decision-node]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            auto first = Registration(fixture.probe);
            auto second = first;
            second.contract.descriptor.type = MakeIdentity<DecisionNodeTypeId>(102);
            auto firstAdmission = std::make_shared<std::atomic_bool>(true);
            auto secondAdmission = std::make_shared<std::atomic_bool>(true);
            auto a = DecisionNodeGeneration::Capture({&first, 1}, firstAdmission);
            auto b = DecisionNodeGeneration::Capture({&second, 1}, secondAdmission);
            REQUIRE(a.HasValue());
            REQUIRE(b.HasValue());
            const std::array reversed{b.Value(), a.Value()};
            auto composed = DecisionNodeGeneration::Compose(reversed);
            REQUIRE(composed.HasValue());
            CHECK(composed.Value()->Contracts()[0].descriptor.type.Value() == 101);
            CHECK(composed.Value()->Contracts()[1].descriptor.type.Value() == 102);
            const std::array duplicate{a.Value(), a.Value()};
            CHECK(DecisionNodeGeneration::Compose(duplicate).HasError());
            const std::array nested{composed.Value()};
            CHECK(DecisionNodeGeneration::Compose(nested).HasError());
            secondAdmission->store(false);
            CHECK_FALSE(composed.Value()->CanAdmit());
            CHECK(composed.Value()->CreateExecutor(composed.Value(), *fixture.Bindings(), {}).HasError());
        }

        TEST_CASE("Changed provider blackboard requirements fail binding before any factory runs", "[ai][decision-node]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            auto registration = Registration(fixture.probe);
            registration.contract.descriptor.requirements = {{.key = MakeIdentity<BlackboardKeyId>(99), .source = {__FILE__, __LINE__, 1}}};
            auto captured = DecisionNodeGeneration::Capture({&registration, 1}, std::make_shared<std::atomic_bool>(true));
            REQUIRE(captured.HasValue());
            auto executor = captured.Value()->CreateExecutor(captured.Value(), *fixture.Bindings(), {});
            REQUIRE(executor.HasError());
            CHECK(executor.ErrorValue().code.Value() == AIErrors::DecisionAssetBindingMissing.code.Value());
            CHECK(fixture.probe->destroyed == 0);
        }
    }  // namespace
}  // namespace Horo::AI
