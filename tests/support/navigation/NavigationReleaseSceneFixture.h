#pragma once

#include "Horo/Application/NavigationContentIntegration.h"
#include "Horo/Navigation/NavigationAssetSceneActivation.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace Horo::Application::ContentTestSupport {
    /** @brief Real asynchronous Scene owner with explicitly supplied package provider and optional native composition. */
    class SceneHarness final {
    public:
        SceneHarness(const AdmittedNavigationReleaseContent &content, const AssetCookTargetId &target, const Assets::AssetId id,
                     Navigation::NavigationAssetBackendFactory factory = {})
            : loads(jobs, content.provider), scenes(registry, loads) {
            const auto type = Assets::AssetTypeId::Parse(Assets::NavMeshAssetTypeName);
            const auto source = ProjectPath::Parse("assets/navigation.horoasset");
            const auto sidecar = ProjectPath::Parse("assets/navigation.horoasset.horo");
            REQUIRE(type.HasValue());
            REQUIRE(source.HasValue());
            REQUIRE(sidecar.HasValue());
            REQUIRE(registry.Publish({{id, type.Value(), source.Value(), sidecar.Value()}}).status ==
                    Assets::AssetRegistryBuildStatus::Complete);
            auto createdCache = Assets::AssetPayloadCache::Create(16, 65536);
            REQUIRE(createdCache.HasValue());
            cache = std::move(createdCache).Value();
            auto participant =
                std::make_unique<Navigation::NavigationAssetSceneActivationParticipant>(*cache, target, std::move(factory),
                                                                                        Navigation::NavigationAssetSceneLimits{},
                                                                                        content.expectations);
            navigation = participant.get();
            REQUIRE(scenes.AddActivationParticipant(std::move(participant)).HasValue());
            REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
        }

        SceneHarness(const SceneHarness &) = delete;
        SceneHarness &operator=(const SceneHarness &) = delete;

        ~SceneHarness() {
            scenes.Shutdown();
        }

        /** @brief Pump the actual owner safe point; timed failure remains a test failure, never a skip. */
        [[nodiscard]] std::optional<Error> Activate(Runtime::RuntimeSceneDefinition definition) {
            const auto revision = definition.Revision();
            const auto submitted = scenes.QueuePreparation(std::move(definition));
            if (submitted.HasError())
                return submitted.ErrorValue();
            const Runtime::FrameContext frame{1, {}, 0, 0, {}, false, cancellation.Token()};
            for (std::size_t iteration = 0; iteration < 2000; ++iteration) {
                REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
                if (auto failure = scenes.TakeOperationError())
                    return failure;
                if (scenes.ActiveScene() && scenes.ActiveScene()->DefinitionRevision() == revision)
                    return std::nullopt;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("Bounded navigation package Scene preparation did not complete");
            return std::nullopt;
        }

        Assets::AssetRegistry registry;
        JobSystem jobs{JobSystemConfig{2, 16}};
        Assets::AssetLoadService loads;
        std::unique_ptr<Assets::AssetPayloadCache> cache;
        Runtime::RuntimeSceneService scenes;
        CancellationSource cancellation;
        Navigation::NavigationAssetSceneActivationParticipant *navigation{}; /**< Borrow from scenes; valid through harness shutdown. */
    };
}  // namespace Horo::Application::ContentTestSupport
