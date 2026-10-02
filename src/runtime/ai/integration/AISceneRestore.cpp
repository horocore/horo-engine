#include "AISceneRuntimeDetail.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AISceneActivation.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::AI {
    namespace Detail {
        /** @brief Preallocated replacement for one exact agent/schema generation. */
        struct RestoreAgent final {
            AgentHandle handle;
            Runtime::EntityRef owner;
            BlackboardInstanceBinding expectedSchema;
            BlackboardInstanceBinding replacementSchema;
            std::unique_ptr<BlackboardInstance> blackboard;
            CancellationSource cancellation;
            AiAgentActivationState state{AiAgentActivationState::Disabled};
        };

        /** @brief Process-local restore fences and detached resources; never canonical state. */
        struct AiSceneRestoreState final {
            const AiSceneRuntime *runtime{};
            Runtime::RuntimeSceneView scene;
            AiSceneActivationBinding binding;
            std::uint64_t publication{};
            std::uint64_t revision{};
            CancellationToken cancellation;
            std::vector<RestoreAgent> agents;
        };
    }  // namespace Detail

    namespace {
        /** @brief Checks the exact destination Scene borrow and lifecycle incarnation. */
        [[nodiscard]] Result<void> ValidateScene(const Runtime::RuntimeSceneView scene, const AiSceneActivationBinding binding) {
            if (!binding.IsValid() || !scene.IsCurrent() || scene.RuntimeId() != binding.scene)
                return Result<void>::Failure(MakeError(AIErrors::CanonicalRestoreStale));
            return Result<void>::Success();
        }

        /** @brief Requires live owners and unchanged authored agent/controller bindings. */
        [[nodiscard]] Result<void> ValidateOwner(const Runtime::RuntimeSceneView scene, const AiAgentRuntimeRecord &record) {
            const auto entity = scene.Get(record.owner);
            if (entity.HasError())
                return Result<void>::Failure(WrapError(AIErrors::CanonicalRestoreStale, entity.ErrorValue()));
            const auto *components = entity.Value().components;
            if (components == nullptr || components->aiAgent != record.agent || components->aiController != record.controller)
                return Result<void>::Failure(MakeError(AIErrors::CanonicalRestoreStale));
            return Result<void>::Success();
        }

        /** @brief Validates scene/entity projections before canonical publication; never trusts representation alone. */
        [[nodiscard]] Result<void> ValidateEntityScalar(const Runtime::RuntimeSceneView scene, const BlackboardScalarValue &value) {
            const auto *reference = std::get_if<BlackboardStoredEntityReference>(&value);
            if (reference == nullptr)
                return Result<void>::Success();
            const Runtime::EntityRef entity{Runtime::SceneRuntimeId{reference->sceneIncarnation}, {reference->slot, reference->generation}};
            const auto current = scene.Get(entity);
            if (current.HasError())
                return Result<void>::Failure(WrapError(AIErrors::CanonicalRestoreStale, current.ErrorValue()));
            return Result<void>::Success();
        }

        /** @brief Checks all entity-valued entries, including bounded collection members. */
        [[nodiscard]] Result<void> ValidateEntities(const Runtime::RuntimeSceneView scene, const BlackboardCanonicalState &state) {
            for (const auto &entry : state.entries) {
                if (!entry.value)
                    continue;
                if (const auto *scalar = std::get_if<BlackboardScalarValue>(&*entry.value)) {
                    if (const auto valid = ValidateEntityScalar(scene, *scalar); valid.HasError())
                        return valid;
                } else if (const auto *collection = std::get_if<BlackboardCollectionValue>(&*entry.value)) {
                    if (collection->count > MaximumBlackboardCollectionElements)
                        return Result<void>::Failure(MakeError(AIErrors::CanonicalStateInvalid));
                    for (std::size_t index = 0; index < collection->count; ++index) {
                        if (const auto valid = ValidateEntityScalar(scene, collection->elements[index]); valid.HasError())
                            return valid;
                    }
                }
            }
            return Result<void>::Success();
        }

        /** @brief Captures durable base state without copying task/cancellation/provider/observer identities. */
        [[nodiscard]] Result<AiAgentCanonicalState> CaptureAgent(const Detail::AgentRuntimeState &slot,
                                                                 const Runtime::RuntimeSceneView scene) {
            AiAgentCanonicalState result{.authored = slot.record.agent,
                                         .state = slot.record.state == AiAgentActivationState::Active ? AiCanonicalAgentState::Active
                                                                                                      : AiCanonicalAgentState::Disabled};
            if (slot.record.controller) {
                result.controller = AiBaseControllerCanonicalState{.authored = *slot.record.controller};
                if (slot.blackboard) {
                    auto captured = slot.blackboard->CaptureCanonical();
                    if (captured.HasError())
                        return Result<AiAgentCanonicalState>::Failure(captured.ErrorValue());
                    if (const auto valid = ValidateEntities(scene, captured.Value()); valid.HasError())
                        return Result<AiAgentCanonicalState>::Failure(valid.ErrorValue());
                    result.controller->blackboard = std::move(captured).Value();
                }
            }
            return Result<AiAgentCanonicalState>::Success(std::move(result));
        }

        /** @brief Checks canonical base-agent shape and requires the same authored destination contract. */
        [[nodiscard]] Result<void> ValidateAgentState(const AiAgentCanonicalState &source, const Detail::AgentRuntimeState &destination) {
            if (source.state >= AiCanonicalAgentState::Count || source.authored != destination.record.agent ||
                source.controller.has_value() != destination.record.controller.has_value())
                return Result<void>::Failure(MakeError(AIErrors::CanonicalStateInvalid));
            if (source.controller && source.controller->authored != *destination.record.controller)
                return Result<void>::Failure(MakeError(AIErrors::CanonicalSchemaUnsupported));
            const bool active = source.state == AiCanonicalAgentState::Active;
            const bool hasBlackboard = source.controller && source.controller->blackboard.has_value();
            if (active != hasBlackboard || (active && (!source.authored.enabled || !source.controller->authored.enabled)))
                return Result<void>::Failure(MakeError(AIErrors::CanonicalStateInvalid));
            return Result<void>::Success();
        }

        /** @brief Materializes one completely validated destination blackboard; live storage remains untouched. */
        [[nodiscard]] Result<Detail::RestoreAgent> StageAgent(const AiAgentCanonicalState &source,
                                                              const Detail::AgentRuntimeState &destination,
                                                              const Runtime::RuntimeSceneView scene,
                                                              const std::span<const BlackboardSchemaMigration> migrations,
                                                              const AiCapabilitySet available) {
            if (const auto valid = ValidateAgentState(source, destination); valid.HasError())
                return Result<Detail::RestoreAgent>::Failure(valid.ErrorValue());
            Detail::RestoreAgent staged{.handle = destination.record.handle,
                                        .owner = destination.record.owner,
                                        .expectedSchema = destination.blackboardBinding,
                                        .replacementSchema = destination.blackboardBinding};
            if (staged.replacementSchema.IsValid()) {
                const auto generation = AdvanceAiRuntimeGeneration(staged.replacementSchema.instanceGeneration);
                if (generation.HasError())
                    return Result<Detail::RestoreAgent>::Failure(generation.ErrorValue());
                staged.replacementSchema.instanceGeneration = generation.Value();
            }
            if (source.state == AiCanonicalAgentState::Disabled)
                return Result<Detail::RestoreAgent>::Success(std::move(staged));
            if (!destination.schema || !staged.replacementSchema.IsValid())
                return Result<Detail::RestoreAgent>::Failure(MakeError(AIErrors::CanonicalSchemaUnsupported));
            if ((source.controller->authored.requiredCapabilities.bits & ~available.bits) != 0)
                return Result<Detail::RestoreAgent>::Failure(MakeError(AIErrors::CapabilityUnavailable));
            auto migrated = MigrateCanonicalBlackboard(*source.controller->blackboard, *destination.schema, migrations);
            if (migrated.HasError())
                return Result<Detail::RestoreAgent>::Failure(migrated.ErrorValue());
            if (const auto valid = ValidateEntities(scene, migrated.Value()); valid.HasError())
                return Result<Detail::RestoreAgent>::Failure(valid.ErrorValue());
            auto blackboard = BlackboardInstance::CreateFromCanonical(staged.replacementSchema, destination.schema, migrated.Value());
            if (blackboard.HasError())
                return Result<Detail::RestoreAgent>::Failure(blackboard.ErrorValue());
            staged.blackboard = std::move(blackboard).Value();
            staged.state = AiAgentActivationState::Active;
            return Result<Detail::RestoreAgent>::Success(std::move(staged));
        }

        /** @brief Requires a complete uniquely identified population and supported canonical version. */
        [[nodiscard]] Result<void> ValidatePopulation(const AiCanonicalState &state, const Detail::AiSceneRuntimeState &destination,
                                                      const std::size_t maximumAgents) {
            if (state.version != CurrentAiCanonicalStateVersion)
                return Result<void>::Failure(MakeError(AIErrors::CanonicalSchemaUnsupported));
            const auto count = static_cast<std::size_t>(std::ranges::count_if(destination.agents, [](const auto &agent) {
                return !agent.retired;
            }));
            if (state.agents.size() > maximumAgents || state.agents.size() != count)
                return Result<void>::Failure(MakeError(AIErrors::CanonicalStateInvalid));
            AgentId previous;
            for (const auto &agent : state.agents) {
                if (!agent.authored.agent.IsValid() || (previous.IsValid() && agent.authored.agent <= previous))
                    return Result<void>::Failure(MakeError(AIErrors::CanonicalStateInvalid));
                previous = agent.authored.agent;
            }
            return Result<void>::Success();
        }
    }  // namespace

    AiSceneRestoreCandidate::AiSceneRestoreCandidate(std::unique_ptr<Detail::AiSceneRestoreState> state) noexcept
        : state_(std::move(state)) {}

    AiSceneRestoreCandidate::~AiSceneRestoreCandidate() = default;

    /** @copydoc AiSceneRuntime::CaptureCanonicalState */
    Result<AiCanonicalState> AiSceneRuntime::CaptureCanonicalState(const Runtime::RuntimeSceneView scene) const {
        if (shutdown_ || !active_)
            return Result<AiCanonicalState>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        if (const auto valid = ValidateScene(scene, active_->binding); valid.HasError())
            return Result<AiCanonicalState>::Failure(valid.ErrorValue());
        try {
            AiCanonicalState state;
            state.agents.reserve(active_->agents.size());
            for (const auto &slot : active_->agents) {
                if (slot.retired)
                    continue;
                if (const auto valid = ValidateOwner(scene, slot.record); valid.HasError())
                    return Result<AiCanonicalState>::Failure(valid.ErrorValue());
                auto captured = CaptureAgent(slot, scene);
                if (captured.HasError())
                    return Result<AiCanonicalState>::Failure(captured.ErrorValue());
                state.agents.push_back(std::move(captured).Value());
            }
            std::ranges::sort(state.agents, {}, [](const auto &agent) {
                return agent.authored.agent;
            });
            return Result<AiCanonicalState>::Success(std::move(state));
        } catch (const std::bad_alloc &) {
            return Result<AiCanonicalState>::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
        }
    }

    /** @copydoc AiSceneRuntime::PrepareRestoreAtSafePoint */
    Result<std::unique_ptr<AiSceneRestoreCandidate>> AiSceneRuntime::PrepareRestoreAtSafePoint(
        const Runtime::RuntimeSceneView scene, const AiSceneActivationBinding expectedBinding, const AiCanonicalState &state,
        const std::span<const BlackboardSchemaMigration> migrations, CancellationToken cancellation) {
        using RestoreResult = Result<std::unique_ptr<AiSceneRestoreCandidate>>;
        if (shutdown_ || !active_)
            return RestoreResult::Failure(MakeError(AIErrors::RuntimeUnavailable));
        if (cancellation.IsCancellationRequested())
            return RestoreResult::Failure(MakeError(AIErrors::CanonicalRestoreCancelled));
        if (const auto valid = ValidateScene(scene, expectedBinding); valid.HasError())
            return RestoreResult::Failure(valid.ErrorValue());
        if (expectedBinding != active_->binding || active_->revision == std::numeric_limits<std::uint64_t>::max())
            return RestoreResult::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        if (migrations.size() > MaximumAiSchemaMigrations)
            return RestoreResult::Failure(MakeError(AIErrors::CanonicalStateInvalid));
        if (const auto valid = ValidatePopulation(state, *active_, settings_.maximumAgents); valid.HasError())
            return RestoreResult::Failure(valid.ErrorValue());
        try {
            auto staged = std::make_unique<Detail::AiSceneRestoreState>();
            staged->runtime = this;
            staged->scene = scene;
            staged->binding = expectedBinding;
            staged->publication = activePublicationToken_;
            staged->revision = active_->revision;
            staged->cancellation = std::move(cancellation);
            staged->agents.reserve(state.agents.size());
            for (const auto &slot : active_->agents) {
                if (slot.retired)
                    continue;
                if (staged->cancellation.IsCancellationRequested())
                    return RestoreResult::Failure(MakeError(AIErrors::CanonicalRestoreCancelled));
                if (const auto valid = ValidateOwner(scene, slot.record); valid.HasError())
                    return RestoreResult::Failure(valid.ErrorValue());
                const auto source = std::ranges::lower_bound(state.agents, slot.record.agent.agent, {}, [](const auto &agent) {
                    return agent.authored.agent;
                });
                if (source == state.agents.end() || source->authored.agent != slot.record.agent.agent)
                    return RestoreResult::Failure(MakeError(AIErrors::CanonicalStateInvalid));
                auto agent = StageAgent(*source, slot, scene, migrations, settings_.availableCapabilities);
                if (agent.HasError())
                    return RestoreResult::Failure(agent.ErrorValue());
                staged->agents.push_back(std::move(agent).Value());
            }
            // Private construction keeps forged/partial restore candidates outside the public API.
            return RestoreResult::Success(std::unique_ptr<AiSceneRestoreCandidate>{new AiSceneRestoreCandidate{std::move(staged)}});
        } catch (const std::bad_alloc &) {
            return RestoreResult::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
        }
    }

    /** @copydoc AiSceneRuntime::CommitRestoreAtSafePoint */
    Result<void> AiSceneRuntime::CommitRestoreAtSafePoint(std::unique_ptr<AiSceneRestoreCandidate> candidate) {
        if (shutdown_ || !active_)
            return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        if (!candidate || !candidate->state_ || candidate->state_->runtime != this)
            return Result<void>::Failure(MakeError(AIErrors::CanonicalStateInvalid));
        auto &staged = *candidate->state_;
        if (staged.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(AIErrors::CanonicalRestoreCancelled));
        if (const auto valid = ValidateScene(staged.scene, staged.binding); valid.HasError())
            return valid;
        if (staged.binding != active_->binding || staged.publication != activePublicationToken_ || staged.revision != active_->revision)
            return Result<void>::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        for (const auto &agent : staged.agents) {
            const auto *slot = Detail::FindAgent(*active_, agent.handle);
            if (slot == nullptr || slot->record.owner != agent.owner || slot->blackboardBinding != agent.expectedSchema)
                return Result<void>::Failure(MakeError(AIErrors::CanonicalRestoreStale));
            if (const auto valid = ValidateOwner(staged.scene, slot->record); valid.HasError())
                return valid;
        }
        // All allocations and fallible checks finish before the first live mutation.
        for (auto &agent : staged.agents) {
            auto &slot = *Detail::FindAgent(*active_, agent.handle);
            Detail::CancelOwnedWork(slot);
            slot.cancellation = std::move(agent.cancellation);
            slot.blackboardBinding = agent.replacementSchema;
            slot.blackboard = std::move(agent.blackboard);
            slot.record.state = agent.state;
            slot.record.hasBlackboard = slot.blackboard != nullptr;
            if (slot.record.hasBlackboard)
                slot.record.stagedCapabilities = slot.record.controller->requiredCapabilities;
        }
        ++active_->revision;
        return Result<void>::Success();
    }
}  // namespace Horo::AI
