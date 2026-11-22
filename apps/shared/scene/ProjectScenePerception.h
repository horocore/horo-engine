#pragma once
/** @file ProjectScenePerception.h
 * @brief Project-authored Perception selection and matched Scene position evidence.
 */
#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/AITaskScheduler.h"
#include "Horo/AI/PerceptionSenseProvider.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Platform/ProjectReadFiles.h"

#include <string_view>

namespace Horo::Application::Internal {
    /** @brief Inert stable selector; a frozen registry owns implementations and stimulus declarations. */
    struct ProjectPerceptionControllerSelection final {
        AI::ControllerTypeId controller;
        std::vector<AI::PerceptionListenerTypeId> listeners;
        std::vector<AI::SenseTypeId> sources;
        AI::PerceptionParticipantFilter participant;
        AI::PerceptionListenerFilter listenerFilter;
        std::vector<AI::DecisionWakePerceptionDependency> decisionWatches;
        std::optional<AI::PerceptionBlackboardProjection> blackboardProjection;
    };

    /** @brief Stable listener authored for exactly one registered custom sense. */
    struct ProjectPerceptionListenerSelection final {
        AI::PerceptionListenerTypeId listener;
        AI::SenseTypeId sense;
    };

    /** @brief Explicit written native execution selection. The generation itself is bound only after module startup. */
    struct ProjectNativePerceptionPolicy final {
        static constexpr auto capabilities = AI::PerceptionCapabilitySet::Of(AI::PerceptionCapability::SpatialCandidateQuery);
        std::vector<ProjectPerceptionListenerSelection> listeners;
        std::uint64_t radiusMillimeters{};
        std::uint64_t visibleLayers{};
        std::size_t workUnitsPerTick{};
        AI::AiScenePerceptionScheduling scheduling{AI::AiScenePerceptionScheduling::FullPopulation};
    };

    /** @brief Project-authored spatial selection tied to one admitted cooked Scene definition. */
    struct ProjectScenePerceptionPolicy final {
        Assets::AssetId asset;
        Runtime::SceneDefinitionId definition;
        Runtime::SceneDefinitionRevision revision;
        Math::WorldCoordinate64 origin;
        std::vector<ProjectPerceptionControllerSelection> controllers;
        std::optional<ProjectNativePerceptionPolicy> native;
        std::optional<AI::PerceptionFilteringPolicy> filtering;
    };

    /** @brief Reports whether the selected project explicitly opted into reactive cooked execution. */
    [[nodiscard]] bool SelectsReactiveDecisions(const ProjectScenePerceptionPolicy &policy) noexcept;

    /** @brief Reads and freezes explicit project policy for the exact verified cooked Scene selection.
     * @param files Project-owned read boundary. @param context Exact load deadline and project identity.
     * @param path Explicit project-relative settings file. @param asset Full selected cooked asset identity.
     * @param definition Definition decoded by the committed asset owner.
     * @param workUnitsLimit Project ceiling for the Scene's native execution budget. @return Frozen policy or typed failure.
     */
    [[nodiscard]] Result<std::shared_ptr<const ProjectScenePerceptionPolicy>> CaptureProjectScenePerception(
        const Platform::IProjectReadFiles &files, const Platform::ProjectReadContext &context, std::string_view path, Assets::AssetId asset,
        const Runtime::RuntimeSceneDefinition &definition, std::size_t workUnitsLimit = AI::AiTaskSchedulerSettings::HardWorkUnits);

    /** @brief Combines authored listener identities with the prepared image's exact inert sense/stimulus contracts.
     * @param policy Explicit native listener declarations. @param contracts Owner-copied inert metadata from one prepared image.
     * @return Immutable registry or typed descriptor/capability failure without starting a module or constructing a sense.
     */
    [[nodiscard]] Result<std::shared_ptr<const AI::PerceptionDescriptorRegistry>> FreezeProjectNativePerceptionRegistry(
        const ProjectNativePerceptionPolicy &policy, std::span<const AI::PerceptionSenseContract> contracts);

    /** @brief Resolves durable controller selections against exactly the admitted controller and descriptor catalogs.
     * @param policy Frozen project selection. @param registry Exact frozen Perception descriptor registry.
     * @param catalog Complete immutable controller descriptors. @return Owned catalog or original typed refusal.
     */
    [[nodiscard]] Result<std::vector<AI::AiControllerDescriptor>> BindProjectPerceptionEndpoints(
        const ProjectScenePerceptionPolicy &policy, std::shared_ptr<const AI::PerceptionDescriptorRegistry> registry,
        std::span<const AI::AiControllerDescriptor> catalog);

    /** @brief Builds real global position capture callbacks for the existing Scene activation and structural owners. */
    struct ProjectPerceptionPositionSources final {
        AI::AiScenePerceptionPositionSource scene;
        AI::AiScenePerceptionHierarchyPositionSource group;
    };

    /** @brief Identity of a host projection already matched to the exact committed cooked Scene. */
    struct ProjectPerceptionSceneProjection final {
        Runtime::SceneDefinitionId definition;
        Runtime::SceneDefinitionRevision revision;
    };

    /** @brief Retains the exact project origin and selected definition on both capture paths.
     * @param policy Frozen project read retained by the callbacks.
     * @param projection Optional verified live projection identity when the host uses its own definition revision.
     * @return Complete live and prospective capture sources.
     */
    [[nodiscard]] ProjectPerceptionPositionSources MakeProjectPerceptionPositionSources(
        std::shared_ptr<const ProjectScenePerceptionPolicy> policy,
        std::optional<ProjectPerceptionSceneProjection> projection = std::nullopt);

    /** @brief Resolves a live entity's hierarchy to exact millimeters relative to the supplied world origin. */
    [[nodiscard]] Result<Math::WorldCoordinate64> CaptureProjectSceneWorldPosition(const Runtime::RuntimeSceneView &scene,
                                                                                   Runtime::EntityRef entity,
                                                                                   Math::WorldCoordinate64 origin);
}  // namespace Horo::Application::Internal
