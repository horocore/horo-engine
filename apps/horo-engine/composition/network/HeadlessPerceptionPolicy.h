#pragma once
/** @file HeadlessPerceptionPolicy.h
 * @brief Project-authored Perception selection and matched Scene position evidence for the headless owner.
 */
#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/PerceptionSenseProvider.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Platform/ProjectReadFiles.h"

#include <string_view>

namespace Horo::Application::Internal {
    /** @brief Inert stable selector; a frozen registry owns implementations and stimulus declarations. */
    struct HeadlessPerceptionControllerSelection final {
        AI::ControllerTypeId controller;
        std::vector<AI::PerceptionListenerTypeId> listeners;
        std::vector<AI::SenseTypeId> sources;
        AI::PerceptionParticipantFilter participant;
    };

    /** @brief Stable listener authored for exactly one registered custom sense. */
    struct HeadlessPerceptionListenerSelection final {
        AI::PerceptionListenerTypeId listener;
        AI::SenseTypeId sense;
    };

    /** @brief Explicit written native execution selection. The generation itself is bound only after module startup. */
    struct HeadlessNativePerceptionPolicy final {
        static constexpr auto capabilities = AI::PerceptionCapabilitySet::Of(AI::PerceptionCapability::SpatialCandidateQuery);
        std::vector<HeadlessPerceptionListenerSelection> listeners;
        std::uint64_t radiusMillimeters{};
        std::uint64_t visibleLayers{};
        std::size_t workUnitsPerTick{};
    };

    /** @brief Project-authored spatial selection tied to one admitted cooked Scene definition. */
    struct HeadlessPerceptionPolicy final {
        Assets::AssetId asset;
        Runtime::SceneDefinitionId definition;
        Runtime::SceneDefinitionRevision revision;
        Math::WorldCoordinate64 origin;
        std::vector<HeadlessPerceptionControllerSelection> controllers;
        std::optional<HeadlessNativePerceptionPolicy> native;
    };

    /** @brief Reads and freezes explicit project policy for the exact verified cooked Scene selection.
     * @param files Project-owned read boundary. @param context Exact load deadline and project identity.
     * @param path Explicit project-relative settings file. @param asset Full selected cooked asset identity.
     * @param definition Definition decoded by the committed asset owner. @return Frozen policy or typed failure.
     */
    [[nodiscard]] Result<std::shared_ptr<const HeadlessPerceptionPolicy>> CaptureHeadlessPerceptionPolicy(
        const Platform::IProjectReadFiles &files, const Platform::ProjectReadContext &context, std::string_view path, Assets::AssetId asset,
        const Runtime::RuntimeSceneDefinition &definition);

    /** @brief Combines authored listener identities with the prepared image's exact inert sense/stimulus contracts.
     * @param policy Explicit native listener declarations. @param contracts Owner-copied inert metadata from one prepared image.
     * @return Immutable registry or typed descriptor/capability failure without starting a module or constructing a sense.
     */
    [[nodiscard]] Result<std::shared_ptr<const AI::PerceptionDescriptorRegistry>> FreezeHeadlessNativePerceptionRegistry(
        const HeadlessNativePerceptionPolicy &policy, std::span<const AI::PerceptionSenseContract> contracts);

    /** @brief Resolves durable controller selections against exactly the admitted controller and descriptor catalogs.
     * @param policy Frozen project selection. @param registry Exact frozen Perception descriptor registry.
     * @param catalog Complete immutable controller descriptors. @return Owned catalog or original typed refusal.
     */
    [[nodiscard]] Result<std::vector<AI::AiControllerDescriptor>> BindHeadlessPerceptionEndpoints(
        const HeadlessPerceptionPolicy &policy, std::shared_ptr<const AI::PerceptionDescriptorRegistry> registry,
        std::span<const AI::AiControllerDescriptor> catalog);

    /** @brief Builds real global position capture callbacks for the existing Scene activation and structural owners. */
    struct HeadlessPerceptionPositionSources final {
        AI::AiScenePerceptionPositionSource scene;
        AI::AiScenePerceptionGroupPositionSource group;
    };

    /** @brief Retains the exact project origin and selected definition on both capture paths.
     * @param policy Frozen project read retained by the callbacks. @return Complete live and prospective capture sources.
     */
    [[nodiscard]] HeadlessPerceptionPositionSources MakeHeadlessPerceptionPositionSources(
        std::shared_ptr<const HeadlessPerceptionPolicy> policy);

    /** @brief Resolves a live entity's hierarchy to exact millimeters relative to the supplied world origin. */
    [[nodiscard]] Result<Math::WorldCoordinate64> CaptureHeadlessWorldPosition(const Runtime::RuntimeSceneView &scene,
                                                                               Runtime::EntityRef entity, Math::WorldCoordinate64 origin);
}  // namespace Horo::Application::Internal
