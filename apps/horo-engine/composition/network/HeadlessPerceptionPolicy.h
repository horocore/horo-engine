#pragma once
/** @file HeadlessPerceptionPolicy.h
 * @brief Project-authored Perception selection and matched Scene position evidence for the headless owner.
 */
#include "Horo/AI/AISceneActivation.h"
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

    /** @brief Project-authored spatial selection tied to one admitted cooked Scene definition. */
    struct HeadlessPerceptionPolicy final {
        Assets::AssetId asset;
        Runtime::SceneDefinitionId definition;
        Runtime::SceneDefinitionRevision revision;
        Math::WorldCoordinate64 origin;
        std::vector<HeadlessPerceptionControllerSelection> controllers;
    };

    /** @brief Reads and freezes explicit project policy for the exact verified cooked Scene selection.
     * @param files Project-owned read boundary. @param context Exact load deadline and project identity.
     * @param path Explicit project-relative settings file. @param asset Full selected cooked asset identity.
     * @param definition Definition decoded by the committed asset owner. @return Frozen policy or typed failure.
     */
    [[nodiscard]] Result<std::shared_ptr<const HeadlessPerceptionPolicy>> CaptureHeadlessPerceptionPolicy(
        const Platform::IProjectReadFiles &files, const Platform::ProjectReadContext &context, std::string_view path,
        Assets::AssetId asset, const Runtime::RuntimeSceneDefinition &definition);

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
}  // namespace Horo::Application::Internal
