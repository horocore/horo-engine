#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Navigation/NavigationSceneActivation.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace Horo::Navigation {
    namespace {
        using Runtime::RuntimeEntityDefinition;
        using Runtime::RuntimeSceneDefinition;
        using Runtime::SceneDefinitionBuilder;
        using Runtime::SceneDefinitionId;
        using Runtime::SceneDefinitionRevision;

        Runtime::NavigationAgentComponent Agent(const std::uint64_t profile, const std::uint64_t filter) {
            return {.profile = NavigationAgentProfileId::Create(profile).Value(),
                    .filter = NavigationFilterId::Create(filter).Value(),
                    .radiusOverride = 0.5F};
        }

        RuntimeSceneDefinition Definition(const std::uint64_t revision, const std::span<const std::uint64_t> objects,
                                          const bool addAgents) {
            SceneDefinitionBuilder builder{SceneDefinitionId{4}, SceneDefinitionRevision{revision}};
            for (std::size_t index = 0; index < objects.size(); ++index) {
                RuntimeEntityDefinition entity{.object = Runtime::SceneObjectId{objects[index]}};
                if (addAgents)
                    entity.components.navigationAgent = Agent(index + 1, index + 11);
                builder.Add(std::move(entity));
            }
            return std::move(builder).Build().Value();
        }

        Runtime::FrameContext Context(const CancellationToken &token) {
            return Runtime::FrameContext{1, {}, 0.0, 0, {}, false, token};
        }

        class GateCandidate final : public Runtime::SceneActivationCandidate {
        public:
            explicit GateCandidate(const bool *fail) noexcept : fail_(fail) {}

            [[nodiscard]] Result<void> ValidatePublication() const override {
                return *fail_ ? Result<void>::Failure(MakeError(NavigationErrors::AgentRegistryStale)) : Result<void>::Success();
            }

            void Shutdown() noexcept override {}

        private:
            const bool *fail_{};
        };

        class GateParticipant final : public Runtime::SceneActivationParticipant {
        public:
            explicit GateParticipant(bool &fail) noexcept : fail_(&fail) {}

            [[nodiscard]] Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &,
                                                                                             Runtime::RuntimeSceneView) override {
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(std::make_unique<GateCandidate>(fail_));
            }

        private:
            bool *fail_{};
        };
    }  // namespace

    TEST_CASE("Navigation scene activation publishes only after aggregate validation and retires old handles",
              "[unit][runtime][scene][navigation][activation]") {
        auto registry = std::move(NavigationAgentRegistry::Create({.maximumAgents = 2})).Value();
        bool gateFails = false;
        Runtime::RuntimeSceneService service;
        REQUIRE(service.AddActivationParticipant(std::make_unique<NavigationSceneActivationParticipant>(registry)).HasValue());
        REQUIRE(service.AddActivationParticipant(std::make_unique<GateParticipant>(gateFails)).HasValue());

        CancellationSource cancellation;
        REQUIRE(service.Startup(cancellation.Token()).HasValue());
        const std::array<std::uint64_t, 1> firstObject{101};
        REQUIRE(service.QueuePreparation(Definition(1, firstObject, true)).HasValue());
        REQUIRE(service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        const auto firstSnapshot = registry.Snapshot();
        REQUIRE(firstSnapshot.HasValue());
        REQUIRE(firstSnapshot.Value().Agents().size() == 1);
        const CrowdAgentHandle firstHandle = firstSnapshot.Value().Agents().front().handle;

        gateFails = true;
        const std::array<std::uint64_t, 1> replacementObjects{202};
        REQUIRE(service.QueuePreparation(Definition(2, replacementObjects, false)).HasValue());
        REQUIRE(service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        REQUIRE(service.TakeOperationError().has_value());
        REQUIRE(service.ActiveScene()->RuntimeId() == Runtime::SceneRuntimeId{1});
        REQUIRE(registry.Find(firstHandle).HasValue());
        REQUIRE(registry.ActiveBinding().Value().scene == NavigationSceneRuntimeId::Create(1).Value());

        gateFails = false;
        REQUIRE(service.QueuePreparation(Definition(3, replacementObjects, false)).HasValue());
        REQUIRE(service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        REQUIRE(service.ActiveScene()->RuntimeId() == Runtime::SceneRuntimeId{3});
        REQUIRE(registry.Find(firstHandle).HasError());
        REQUIRE(registry.Snapshot().Value().Agents().empty());
    }

    TEST_CASE("Navigation scene activation rejects an over-capacity candidate without changing the active scene",
              "[unit][runtime][scene][navigation][activation]") {
        auto registry = std::move(NavigationAgentRegistry::Create({.maximumAgents = 1})).Value();
        Runtime::RuntimeSceneService service;
        REQUIRE(service.AddActivationParticipant(std::make_unique<NavigationSceneActivationParticipant>(registry)).HasValue());
        CancellationSource cancellation;
        REQUIRE(service.Startup(cancellation.Token()).HasValue());

        const std::array<std::uint64_t, 1> firstObject{301};
        REQUIRE(service.QueuePreparation(Definition(1, firstObject, true)).HasValue());
        REQUIRE(service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        const Runtime::SceneRuntimeId activeId = service.ActiveScene()->RuntimeId();
        const auto activeBinding = registry.ActiveBinding().Value();

        const std::array<std::uint64_t, 2> tooManyObjects{401, 402};
        REQUIRE(service.QueuePreparation(Definition(2, tooManyObjects, true)).HasError());
        REQUIRE(service.ActiveScene()->RuntimeId() == activeId);
        REQUIRE(registry.ActiveBinding().Value() == activeBinding);
        REQUIRE(registry.Snapshot().Value().Agents().size() == 1);
    }
}  // namespace Horo::Navigation
