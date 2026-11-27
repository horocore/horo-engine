#pragma once
/** @file HeadlessReducedPopulation.h
 * @brief Committed cooked templates and bounded Scene transfers for the authoritative headless product.
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
                                                              std::vector<AI::AiReducedAgent>(maximum), {}, {}});
        }

        /** @brief Queues a bounded promotion using the existing aggregate Scene transaction. */
        Result<void> QueuePromotions(Runtime::RuntimeSceneService &scenes, const WorldStreaming::OriginFrame &origin,
                                      const Math::WorldCoordinate64 &anchor,
                                      const std::size_t available, const Runtime::SceneStructuralAdmission &admission) {
            if (pending || available == 0 || !scenes.CanQueueStructuralCommands())
                return Result<void>::Success();
            const auto evaluated = population->EvaluatePromotions(anchor.Millimeters(), candidates);
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
            return QueueGroup(scenes, std::move(commands));
        }

        /** @brief Retires only exact suspended manifest owners that still qualify at the structural cutoff. */
        Result<void> QueueDemotions(Runtime::RuntimeSceneService &scenes, const Runtime::RuntimeSceneView &view,
                                     const std::span<const AI::AgentHandle> scheduled, AI::AiTaskScheduler &scheduler,
                                     const AI::AiRuntimeCapability &capability, const Runtime::SceneStructuralAdmission &admission,
                                     const auto &outsideSuspensionRadius) {
            if (pending || !scenes.CanQueueStructuralCommands())
                return Result<void>::Success();
            demotionCandidates.clear();
            for (const auto agent : scheduled) {
                const auto tier = scheduler.SimulationTier(agent);
                if (tier.HasError() || tier.Value() != AI::AiSimulationTier::Suspended)
                    continue;
                const auto entity = capability.ResolveAgentOwner(agent);
                if (entity.HasValue() && OwnsTemplate(view, entity.Value()) && outsideSuspensionRadius(entity.Value()))
                    demotionCandidates.push_back({entity.Value()});
            }
            if (demotionCandidates.empty())
                return Result<void>::Success();
            ProtectParents(view);
            std::vector<Runtime::EntityRef> entities;
            for (const auto &candidate : demotionCandidates) {
                if (entities.size() == candidates.size())
                    break;
                if (!candidate.hasChild)
                    entities.push_back(candidate.entity);
            }
            if (entities.empty())
                return Result<void>::Success();
            Runtime::SceneCommandBuffer commands;
            const auto destroyed = commands.DestroyGroup(std::move(entities), admission);
            return destroyed.HasError() ? destroyed : QueueGroup(scenes, std::move(commands));
        }

        /** @brief Reads the exact aggregate outcome and frees admission for the next frame. */
        Result<void> ReconcilePublication() {
            if (!pending || !pending->Complete())
                return Result<void>::Success();
            const auto receipt = std::exchange(pending, {});
            return receipt->Failure() ? Result<void>::Failure(*receipt->Failure()) : Result<void>::Success();
        }

        /** @brief Binds the existing AI structural owner to the identities reserved by this cooked population. */
        [[nodiscard]] std::unique_ptr<Runtime::SceneStructuralParticipant> GuardStructuralParticipant(
            std::unique_ptr<Runtime::SceneStructuralParticipant> participant) const {
            return std::make_unique<GuardedParticipant>(std::move(participant), definition, population);
        }

        std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition; /**< Pins every borrowed template. */
        std::vector<const Runtime::RuntimeEntityDefinition *> templates;
        std::shared_ptr<AI::AiReducedPopulation> population;
        std::vector<AI::AiReducedAgent> candidates;
        std::shared_ptr<const Runtime::SceneStructuralReceipt> pending;
        struct DemotionCandidate final {
            Runtime::EntityRef entity;
            bool hasChild{};
        };
        std::vector<DemotionCandidate> demotionCandidates;

    private:
        /** @brief Checks host-owned template identities before the ordinary AI owner prepares a Scene transaction. */
        class GuardedParticipant final : public Runtime::SceneStructuralParticipant {
        public:
            GuardedParticipant(std::unique_ptr<Runtime::SceneStructuralParticipant> participant,
                               std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition,
                               std::shared_ptr<const AI::AiReducedPopulation> population)
                : participant_(std::move(participant)), definition_(std::move(definition)), population_(std::move(population)) {}

            Runtime::SceneStructuralOwner Owner() const noexcept override {
                return participant_->Owner();
            }

            Runtime::SceneStructuralPreparationPhase PreparationPhase() const noexcept override {
                return participant_->PreparationPhase();
            }

            Result<void> ValidateBaselines(const Runtime::RuntimeSceneView &active,
                                           std::span<const Runtime::SceneBaselinePreparationView> baselines) const override {
                for (const auto &baseline : baselines)
                    for (const auto &entity : baseline.definition.Entities())
                        if (FindTemplate(entity.object) || Tracks(entity.components))
                            return Result<void>::Failure(IdentityConflict());
                return participant_->ValidateBaselines(active, baselines);
            }

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &scene,
                                                                               std::span<const Runtime::RuntimeEntityView> created,
                                                                               std::span<const Runtime::EntityRef> destroyed) override {
                return PrepareWithHierarchy(scene, created, destroyed, {});
            }

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> PrepareWithHierarchy(
                const Runtime::RuntimeSceneView &scene, std::span<const Runtime::RuntimeEntityView> created,
                std::span<const Runtime::EntityRef> destroyed, const Runtime::SceneStructuralHierarchyView &hierarchy) override {
                using Output = Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>;
                for (const auto &entity : created)
                    if (!Accepts(entity))
                        return Output::Failure(IdentityConflict());
                return participant_->PrepareWithHierarchy(scene, created, destroyed, hierarchy);
            }

        private:
            /** @brief Either namespace is reserved, and only its exact independent cooked AI binding can use both. */
            bool Accepts(const Runtime::RuntimeEntityView &entity) const noexcept {
                const auto *source = entity.authoredObject ? FindTemplate(*entity.authoredObject) : nullptr;
                if (!source)
                    return !Tracks(*entity.components);
                return !entity.parent && entity.components->aiAgent == source->components.aiAgent &&
                       entity.components->aiController == source->components.aiController;
            }

            /** @brief Resolves a reserved authored object from the immutable Scene's canonical object order. */
            const Runtime::RuntimeEntityDefinition *FindTemplate(const Runtime::SceneObjectId object) const noexcept {
                const auto entities = definition_->Entities();
                const auto found = std::ranges::lower_bound(entities, object, {}, &Runtime::RuntimeEntityDefinition::object);
                return found != entities.end() && found->object == object ? &*found : nullptr;
            }

            /** @brief Identifies a reserved AI identity even when the proposed object is absent or wrong. */
            bool Tracks(const Runtime::RuntimeComponentSet &components) const noexcept {
                return components.aiAgent && population_->Tracks(components.aiAgent->agent);
            }

            /** @brief Reports the rejected ownership without mutating either representation. */
            static Error IdentityConflict() {
                return MakeError(AI::AIErrors::SceneActivationInvalid, "Scene creation conflicts with a reserved reduced agent template.");
            }

            std::unique_ptr<Runtime::SceneStructuralParticipant> participant_;
            std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition_;
            std::shared_ptr<const AI::AiReducedPopulation> population_;
        };

        /** @brief Records the actual structural operation instead of mutating reduced state during selection. */
        Result<void> QueueGroup(Runtime::RuntimeSceneService &scenes, Runtime::SceneCommandBuffer commands) {
            auto queued = scenes.QueueTrackedStructuralCommands(std::move(commands));
            if (queued.HasError())
                return Result<void>::Failure(queued.ErrorValue());
            pending = std::move(queued).Value();
            return Result<void>::Success();
        }

        /** @brief Ensures an exact independent entity belongs to this cooked population template. */
        bool OwnsTemplate(const Runtime::RuntimeSceneView &scene, const Runtime::EntityRef entity) const {
            const auto live = scene.Get(entity);
            if (live.HasError() || live.Value().parent || !live.Value().authoredObject || !live.Value().components->aiAgent)
                return false;
            const auto agent = live.Value().components->aiAgent->agent;
            const auto found = std::ranges::lower_bound(templates, agent, {}, AgentIdentity);
            return found != templates.end() && AgentIdentity(*found) == agent && (*found)->object == live.Value().authoredObject;
        }

        /** @brief A descendant acquired during gameplay keeps its parent canonical. */
        void ProtectParents(const Runtime::RuntimeSceneView &scene) {
            std::ranges::sort(demotionCandidates, {}, &DemotionCandidate::entity);
            for (std::size_t slot = 0; slot < scene.SlotCount(); ++slot) {
                const auto entity = scene.EntityAt(slot);
                if (!entity || !entity->parent)
                    continue;
                const auto parent = std::ranges::lower_bound(demotionCandidates, *entity->parent, {}, &DemotionCandidate::entity);
                if (parent != demotionCandidates.end() && parent->entity == *entity->parent)
                    parent->hasChild = true;
            }
        }

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
