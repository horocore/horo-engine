#pragma once
/** @file HeadlessReducedPopulation.h
 * @brief Committed cooked templates and bounded Scene promotion for the authoritative headless product.
 */
#include "HeadlessCookedDecisionAdmission.h"
#include "Horo/AI/AiReducedPopulation.h"
#include "Horo/WorldStreaming/OriginFrame.h"
#include "ProjectScenePerception.h"

#include <algorithm>
#include <set>

namespace Horo::Application::Internal {
    /** @brief Owns immutable templates, the reduced runtime, and one tracked structural receipt for a single World. */
    class HeadlessReducedPopulation final {
    public:
        /** @brief Validates the complete committed manifest before reserving runtime population state. */
        static Result<HeadlessReducedPopulation> Create(const HeadlessReducedPopulationSelection &selection,
                                                         const Runtime::RuntimeSceneDefinition &root,
                                                         const AI::AiDecisionSimulationProfile &profile,
                                                         const WorldStreaming::OriginFrame &origin) {
            using Output = Result<HeadlessReducedPopulation>;
            const auto &definition = *selection.definition;
            if (definition.Entities().empty() || !definition.AssetDependencies().empty() || !definition.ChildScenes().empty() ||
                definition.Entities().size() > AI::AiReducedPopulationLimits::HardAgents)
                return Output::Failure(MakeError(AI::AIErrors::SceneActivationInvalid,
                                                 "Reduced population must be a nonempty cooked Scene of independent assetless agents."));
            std::set<AI::AgentId> resident;
            for (const auto &entity : root.Entities())
                if (entity.components.aiAgent)
                    resident.insert(entity.components.aiAgent->agent);
            std::vector<const Runtime::RuntimeEntityDefinition *> templates;
            templates.reserve(definition.Entities().size());
            for (const auto &entity : definition.Entities()) {
                if (!IsIndependentAgent(entity) || resident.contains(entity.components.aiAgent->agent) ||
                    std::ranges::binary_search(root.Entities(), entity.object, {}, &Runtime::RuntimeEntityDefinition::object))
                    return Output::Failure(MakeError(AI::AIErrors::SceneActivationInvalid,
                                                     "Reduced population templates must have distinct Scene and agent identities."));
                templates.push_back(&entity);
            }
            std::ranges::sort(templates, {}, AgentIdentity);
            std::vector<AI::AiReducedAgent> rows;
            rows.reserve(templates.size());
            for (const auto *entity : templates) {
                const auto position = WorldStreaming::OriginLocalCoordinate::Create(
                    entity->localTransform.translation, origin.Binding().identity, origin.Binding().generation);
                if (position.HasError())
                    return Output::Failure(position.ErrorValue());
                const auto world = origin.ToGlobal(position.Value());
                if (world.HasError())
                    return Output::Failure(world.ErrorValue());
                rows.push_back({AgentIdentity(entity), world.Value().Millimeters()});
            }
            const auto maximum = std::min(selection.maximumPromotions, rows.size());
            auto created = AI::AiReducedPopulation::Create({rows.size(), maximum, profile.fullDistanceMillimeters}, rows);
            if (created.HasError())
                return Output::Failure(created.ErrorValue());
            return Output::Success(HeadlessReducedPopulation{selection.definition, std::move(templates),
                                                              std::make_shared<AI::AiReducedPopulation>(std::move(created).Value()),
                                                              std::vector<AI::AiReducedAgent>(maximum), {}});
        }

        /** @brief Queues a bounded promotion using the existing aggregate Scene transaction. */
        Result<void> QueuePromotions(Runtime::RuntimeSceneService &scenes, const Runtime::RuntimeSceneView &view,
                                      const WorldStreaming::OriginFrame &origin, const Runtime::SceneObjectId anchor,
                                      const std::size_t available, const Runtime::SceneStructuralAdmission &admission) {
            if (pending || available == 0 || !scenes.CanQueueStructuralCommands())
                return Result<void>::Success();
            const auto entity = view.Find(anchor);
            if (!entity)
                return Result<void>::Success();
            const auto position = CaptureProjectSceneWorldPosition(view, *entity, origin.Origin());
            if (position.HasError())
                return Result<void>::Success();
            const auto evaluated = population->EvaluatePromotions(position.Value().Millimeters(), candidates);
            if (evaluated.HasError())
                return Result<void>::Failure(evaluated.ErrorValue());
            const auto count = std::min(evaluated.Value(), available);
            if (count == 0)
                return Result<void>::Success();
            auto entries = BuildGroup(std::span(candidates).first(count), origin);
            if (entries.HasError())
                return Result<void>::Failure(entries.ErrorValue());
            Runtime::SceneCommandBuffer commands;
            const auto created = commands.CreateGroup(std::move(entries).Value(), {}, admission);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            auto queued = scenes.QueueTrackedStructuralCommands(std::move(commands));
            if (queued.HasError())
                return Result<void>::Failure(queued.ErrorValue());
            pending = std::move(queued).Value();
            return Result<void>::Success();
        }

        /** @brief Reads the exact aggregate outcome and frees admission for the next frame. */
        Result<void> ReconcilePublication() {
            if (!pending || !pending->Complete())
                return Result<void>::Success();
            const auto receipt = std::exchange(pending, {});
            return receipt->Failure() ? Result<void>::Failure(*receipt->Failure()) : Result<void>::Success();
        }

        std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition; /**< Pins every borrowed template. */
        std::vector<const Runtime::RuntimeEntityDefinition *> templates;
        std::shared_ptr<AI::AiReducedPopulation> population;
        std::vector<AI::AiReducedAgent> candidates;
        std::shared_ptr<const Runtime::SceneStructuralReceipt> pending;

    private:
        /** @brief Identifies supported independent canonical agent templates. */
        static bool IsIndependentAgent(const Runtime::RuntimeEntityDefinition &entity) noexcept {
            return !entity.parent && entity.components.aiAgent && entity.components.aiController &&
                   entity.components.aiAgent->agent.IsValid() && entity.components.aiController->enabled;
        }

        /** @brief Projects the stable identity of a verified template. */
        static AI::AgentId AgentIdentity(const Runtime::RuntimeEntityDefinition *entity) noexcept {
            return entity->components.aiAgent->agent;
        }

        /** @brief Restores exact reduced positions into the current Scene coordinate frame. */
        Result<std::vector<Runtime::RuntimeEntityGroupEntry>> BuildGroup(const std::span<const AI::AiReducedAgent> rows,
                                                                        const WorldStreaming::OriginFrame &origin) const {
            using Output = Result<std::vector<Runtime::RuntimeEntityGroupEntry>>;
            std::vector<Runtime::RuntimeEntityGroupEntry> entries;
            entries.reserve(rows.size());
            for (const auto &row : rows) {
                const auto found = std::ranges::lower_bound(templates, row.agent, {}, AgentIdentity);
                HORO_INVARIANT(found != templates.end() && AgentIdentity(*found) == row.agent);
                const auto &source = **found;
                const auto &pos = row.positionMillimeters;
                const auto local = origin.ToLocal(Math::WorldCoordinate64::FromMillimeters(pos[0], pos[1], pos[2]));
                if (local.HasError())
                    return Output::Failure(local.ErrorValue());
                auto transform = source.localTransform;
                transform.translation = local.Value().Value();
                entries.push_back({.info = {.localTransform = transform, .authoredObject = source.object,
                                             .primitiveMesh = source.primitiveMesh, .components = source.components}});
            }
            return Output::Success(std::move(entries));
        }
    };
}  // namespace Horo::Application::Internal
