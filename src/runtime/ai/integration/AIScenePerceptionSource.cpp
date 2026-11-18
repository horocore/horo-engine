#include "Horo/AI/AIScenePerceptionSource.h"

#include "AISceneRuntimeDetail.h"
#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Selects only active agents with admitted Perception descriptors. */
        [[nodiscard]] bool HasActiveEndpoints(const Detail::AgentRuntimeState &agent) noexcept {
            return !agent.retired && agent.record.state == AiAgentActivationState::Active && agent.perceptionEndpoints;
        }

        /** @brief Creates one exact active recipient's native instances under the complete Scene budget. */
        [[nodiscard]] Result<void> PrepareRecipient(const AiScenePerceptionExecution &policy, Detail::AgentRuntimeState &agent,
                                                    std::size_t &remaining) {
            if (!HasActiveEndpoints(agent) || agent.perceptionEndpoints->listeners.empty())
                return Result<void>::Success();
            auto memory = AIPerceptionMemory::Create(agent.record.owner.runtime.value, agent.record.handle, policy.memory);
            if (memory.HasError())
                return Result<void>::Failure(memory.ErrorValue());
            auto recipient = std::make_unique<Detail::SceneAgentPerception>(Detail::SceneAgentPerception{std::move(memory).Value()});
            const auto &endpoints = *agent.perceptionEndpoints;
            const auto listeners = endpoints.registry->Listeners();
            const auto contracts = policy.generation->Contracts();
            recipient->senses.reserve(endpoints.listeners.size());
            for (const auto identity : endpoints.listeners) {
                const auto listener = std::ranges::lower_bound(listeners, identity, {}, [](const auto &entry) {
                    return entry.descriptor.identity;
                });
                const auto sense = listener->descriptor.sense;
                const auto contract = std::ranges::lower_bound(contracts, sense, {}, [](const PerceptionSenseContract &entry) {
                    return entry.sense.identity;
                });
                if (contract == contracts.end() || contract->sense.identity != sense)
                    return Result<void>::Failure(
                        MakeError(AIErrors::PerceptionDependencyMissing, "No admitted native producer owns this listener."));
                const auto units = contract->workUnits + PerceptionSenseGeneration::ValidationWorkUnits + 1;
                if (units > remaining)
                    return Result<void>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
                auto instance = policy.generation->Create(policy.generation, sense, *endpoints.registry, policy.capabilities,
                                                          {agent.record.handle, ProjectPerceptionSource(agent.record.owner), identity,
                                                           agent.cancellation.Token()});
                if (instance.HasError())
                    return Result<void>::Failure(instance.ErrorValue());
                recipient->senses.push_back({std::move(instance).Value(),
                                             {.listener = agent.record.owner,
                                              .sense = sense,
                                              .radiusMillimeters = policy.radiusMillimeters,
                                              .visibleLayers = policy.visibleLayers,
                                              .maximumCandidates = contract->maximumCandidates}});
                recipient->workUnits += units;
                remaining -= units;
            }
            agent.sensing = std::move(recipient);
            return Result<void>::Success();
        }

        /** @brief Checks a weak observation source with the exact current scene generation. */
        [[nodiscard]] bool ViewSourceAlive(void *context, const PerceptionSourceRef &source) {
            const auto &view = *static_cast<const Runtime::RuntimeSceneView *>(context);
            return source.IsValid() && view.IsCurrent() && view.RuntimeId().value == source.sceneIncarnation &&
                   view.Get({view.RuntimeId(), {source.slot, source.generation}}).HasValue();
        }

        /** @brief Appends one fully admitted endpoint using exact host-captured coordinates. */
        void AppendEndpoints(const Detail::AgentRuntimeState &agent, const Math::WorldCoordinate64 position,
                             std::vector<PerceptionSpatialListener> &listeners, std::vector<PerceptionSpatialSource> &sources) {
            const auto &endpoints = *agent.perceptionEndpoints;
            const auto owner = agent.cancellation.Token();
            if (!endpoints.listeners.empty()) {
                PerceptionSpatialListener listener{.entity = agent.record.owner,
                                                   .position = position,
                                                   .filtering = endpoints.participant,
                                                   .admission = endpoints.listenerFilter,
                                                   .ownerCancellation = owner};
                const auto registered = endpoints.registry->Listeners();
                for (const auto identity : endpoints.listeners) {
                    const auto entry = std::ranges::lower_bound(registered, identity, {}, [](const auto &value) {
                        return value.descriptor.identity;
                    });
                    listener.senses[listener.senseCount++] = entry->descriptor.sense;
                }
                listeners.push_back(std::move(listener));
            }
            if (!endpoints.sources.empty()) {
                PerceptionSpatialSource source{.entity = agent.record.owner,
                                               .position = position,
                                               .layers = endpoints.participant.layers,
                                               .senseCount = endpoints.sources.size(),
                                               .team = endpoints.participant.team,
                                               .listenerMask = endpoints.participant.mask,
                                               .visibility = endpoints.participant.visibility,
                                               .ownerCancellation = owner};
                std::ranges::copy(endpoints.sources, source.senses.begin());
                sources.push_back(std::move(source));
            }
        }

        /** @brief Checks the current structural revision rather than trusting a retained entity slot. */
        [[nodiscard]] bool SceneSourceAlive(void *context, const PerceptionSourceRef &source) {
            const auto &scene = *static_cast<Runtime::RuntimeScene *>(context);
            auto view = scene.View();
            return ViewSourceAlive(&view, source);
        }

        /** @brief Owns the matching debug and paired blackboard values before any publication. */
        struct ProjectionCandidate final {
            PerceptionPublicationSnapshot snapshot;
            BlackboardCollectionValue sources{.elementKind = BlackboardValueKind::EntityReference};
            BlackboardCollectionValue positions{.elementKind = BlackboardValueKind::WorldCoordinate};
            std::size_t admittedCount{};

            /** @brief Stages both collections under the same revocable permission. */
            [[nodiscard]] Result<void> Stage(BlackboardWriteBatch &batch, const PerceptionBlackboardProjection &projection,
                                             const std::shared_ptr<const BlackboardValueAuthorization> &authorization) const {
                if (const auto source = batch.Stage({projection.sources, BlackboardValue{sources}, authorization}); source.HasError())
                    return source;
                return batch.Stage({projection.positions, BlackboardValue{positions}, authorization});
            }
        };

        /** @brief Resolves one projection key against the stable schema inventory. */
        [[nodiscard]] Result<std::size_t> CollectionCapacity(const BlackboardSchema &schema, const BlackboardKeyId key,
                                                             const BlackboardValueKind kind) {
            const auto keys = schema.Keys();
            const auto found = std::ranges::lower_bound(keys, key, {}, &BlackboardKeyDescriptor::key);
            if (found == keys.end() || found->key != key)
                return Result<std::size_t>::Failure(MakeError(AIErrors::BlackboardUnknownValueRejected));
            if (found->access != BlackboardKeyAccess::ReadWrite)
                return Result<std::size_t>::Failure(MakeError(AIErrors::BlackboardBatchInvalid));
            if (found->kind != kind || found->cardinality != BlackboardValueCardinality::Collection)
                return Result<std::size_t>::Failure(MakeError(AIErrors::BlackboardValueTypeMismatch));
            return Result<std::size_t>::Success(found->maximumCollectionElements);
        }

        /** @brief Filters remembered subject/provenance and bounds the complete paired projection before writes begin. */
        [[nodiscard]] ProjectionCandidate BuildProjection(const Runtime::RuntimeSceneView &scene,
                                                          const PerceptionSpatialSnapshot &filtering,
                                                          const PerceptionMemorySnapshot &memory,
                                                          const PerceptionPublicationSnapshot &identity, const std::size_t capacity) {
            ProjectionCandidate next{.snapshot = identity};
            for (std::size_t index = 0; index < memory.count; ++index) {
                const auto &record = memory.entries[index];
                if (const auto admits = [&](const PerceptionSourceRef source) {
                    return filtering.Allows(identity.listener, {scene.RuntimeId(), {source.slot, source.generation}}, record.key.sense);
                }; !admits(record.key.source) || !admits(record.provenance))
                    continue;
                if (++next.admittedCount > capacity)
                    continue;
                const auto offset = next.snapshot.memory.count++;
                next.snapshot.memory.entries[offset] = record;
                next.sources.elements[next.sources.count++] =
                    BlackboardStoredEntityReference{record.key.source.sceneIncarnation, record.key.source.slot,
                                                    record.key.source.generation};
                next.positions.elements[next.positions.count++] = record.lastKnownPosition;
            }
            return next;
        }
    }  // namespace

    /** @copydoc Detail::PerceptionGroupPublication::PrepareScene */
    Result<Detail::PerceptionGroupPublication> Detail::PerceptionGroupPublication::PrepareScene(
        PerceptionSpatialBroadphase &spatial, const Runtime::RuntimeSceneView &scene, const std::uint64_t revision,
        const std::span<const AiScenePerceptionPosition> positions, const PerceptionFilteringPolicy &filtering,
        const std::span<const AgentRuntimeState> agents) {
        using Publication = Result<PerceptionGroupPublication>;
        const auto byEntity = &AiScenePerceptionPosition::entity;
        if (positions.size() > MaximumAiSceneAgents || !std::ranges::is_sorted(positions, {}, byEntity) ||
            std::ranges::adjacent_find(positions, {}, byEntity) != positions.end())
            return Publication::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
        try {
            std::vector<PerceptionSpatialListener> listeners;
            std::vector<PerceptionSpatialSource> sources;
            for (const auto &agent : agents) {
                if (!HasActiveEndpoints(agent))
                    continue;
                const auto entity = agent.record.owner;
                const auto position = std::ranges::lower_bound(positions, entity, {}, byEntity);
                if (position == positions.end() || position->entity != entity)
                    return Publication::Failure(MakeError(AIErrors::PerceptionDependencyMissing));
                AppendEndpoints(agent, position->position, listeners, sources);
            }
            auto next = spatial.Prepare(scene, revision, listeners, sources, filtering, {});
            if (next.HasError())
                return Publication::Failure(next.ErrorValue());
            return Publication::Success({&spatial, spatial.Current(), std::move(next).Value(), scene});
        } catch (const std::bad_alloc &) {
            return Publication::Failure(MakeError(AIErrors::AgentCapacityExceeded));
        }
    }

    /** @copydoc Detail::PerceptionGroupPublication::Prepare */
    Result<std::optional<Detail::PerceptionGroupPublication>> Detail::PerceptionGroupPublication::Prepare(
        const AiScenePerceptionGroupInput &input, const Runtime::RuntimeSceneView &scene,
        const std::span<const Runtime::RuntimeEntityView> created, const std::span<const Runtime::EntityRef> destroyed,
        const std::span<const AgentRuntimeState> additions) {
        using Publication = Result<std::optional<PerceptionGroupPublication>>;
        const auto previous = input.spatial.Current();
        const auto retires = [destroyed](const auto &endpoint) {
            return std::ranges::find(destroyed, endpoint.entity) != destroyed.end();
        };
        const bool changes =
            std::ranges::any_of(additions, HasActiveEndpoints) ||
            (previous && (std::ranges::any_of(previous->listeners_, retires) || std::ranges::any_of(previous->sources_, retires)));
        if (!changes)
            return Publication::Success(std::nullopt);
        if (!previous || previous->Scene() != scene.RuntimeId())
            return Publication::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        if (previous->Revision() == std::numeric_limits<std::uint64_t>::max())
            return Publication::Failure(MakeError(AIErrors::PerceptionSpatialLimitExceeded));

        std::vector<Runtime::RuntimeEntityView> prospective;
        for (const auto &agent : additions) {
            if (!HasActiveEndpoints(agent))
                continue;
            const auto found = std::ranges::find(created, agent.record.owner, &Runtime::RuntimeEntityView::entity);
            if (found == created.end())
                return Publication::Failure(MakeError(AIErrors::PerceptionSpatialStale));
            prospective.push_back(*found);
        }
        auto listeners = previous->listeners_;
        auto sources = previous->sources_;
        const auto inactive = [&](const auto &endpoint) {
            return endpoint.ownerCancellation.IsCancellationRequested() || retires(endpoint);
        };
        std::erase_if(listeners, inactive);
        std::erase_if(sources, inactive);
        if (!prospective.empty()) {
            if (!input.positions)
                return Publication::Failure(MakeError(AIErrors::PerceptionDependencyMissing));
            std::ranges::sort(prospective, {}, &Runtime::RuntimeEntityView::entity);
            const auto captured = input.positions(scene, prospective);
            if (captured.HasError())
                return Publication::Failure(captured.ErrorValue());
            const auto &positions = captured.Value();
            if (positions.size() != prospective.size() ||
                !std::ranges::equal(positions, prospective, {}, &AiScenePerceptionPosition::entity, &Runtime::RuntimeEntityView::entity))
                return Publication::Failure(MakeError(AIErrors::PerceptionDependencyMissing));
            for (const auto &agent : additions) {
                if (!HasActiveEndpoints(agent))
                    continue;
                const auto position = std::ranges::lower_bound(positions, agent.record.owner, {}, &AiScenePerceptionPosition::entity);
                AppendEndpoints(agent, position->position, listeners, sources);
            }
        }
        auto next = input.spatial.Prepare(scene, previous->Revision() + 1, listeners, sources, input.filtering, prospective);
        if (next.HasError())
            return Publication::Failure(next.ErrorValue());
        return Publication::Success(PerceptionGroupPublication{&input.spatial, previous, std::move(next).Value(), scene});
    }

    /** @copydoc Detail::PerceptionGroupPublication::IsCurrent */
    bool Detail::PerceptionGroupPublication::IsCurrent() const noexcept {
        return preparedScene.IsCurrent() && owner->Current() == previous;
    }

    /** @copydoc Detail::PerceptionGroupPublication::Publish */
    void Detail::PerceptionGroupPublication::Publish() noexcept {
        owner->PublishPrepared(std::move(next));
        previous.reset();
        preparedScene = {};
    }

    /** @copydoc Detail::PerceptionGroupPublication::RetireScene */
    void Detail::PerceptionGroupPublication::RetireScene(const Runtime::SceneRuntimeId scene) noexcept {
        const auto current = owner->Current();
        if (current && current->Scene() == scene)
            owner->PublishPrepared({});
    }

    /** @copydoc Detail::PreparePerceptionSenses */
    Result<void> Detail::PreparePerceptionSenses(const AiScenePerceptionExecution &policy,
                                                 const std::span<const AgentRuntimeState> residents,
                                                 const std::span<AgentRuntimeState> additions,
                                                 const std::span<const std::size_t> removals) {
        constexpr auto allCapabilityBits = (std::uint32_t{1} << static_cast<unsigned>(PerceptionCapability::Count)) - 1;
        if (!policy.generation || !policy.generation->CanAdmit())
            return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        if (policy.memory.fixedStep.count() <= 0)
            return Result<void>::Failure(MakeError(AIErrors::PerceptionMemoryInvalid));
        if (policy.radiusMillimeters > PerceptionSpatialLimits::RadiusMillimeters || policy.visibleLayers == 0 ||
            policy.workUnitsPerTick == 0 || policy.workUnitsPerTick > AiTaskSchedulerSettings::HardWorkUnits ||
            (policy.capabilities.bits & ~allCapabilityBits) != 0)
            return Result<void>::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
        auto remaining = policy.workUnitsPerTick;
        for (std::size_t index = 0; index < residents.size(); ++index) {
            const auto &agent = residents[index];
            if (!agent.sensing || std::ranges::binary_search(removals, index))
                continue;
            if (agent.sensing->workUnits > remaining)
                return Result<void>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
            remaining -= agent.sensing->workUnits;
        }
        for (auto &agent : additions)
            if (auto prepared = PrepareRecipient(policy, agent, remaining); prepared.HasError())
                return prepared;
        return Result<void>::Success();
    }

    /** @copydoc Detail::ValidatePerceptionCapacity */
    Result<void> Detail::ValidatePerceptionCapacity(const std::span<const AgentRuntimeState> residents,
                                                    const std::span<const AgentRuntimeState> additions,
                                                    const std::span<const std::size_t> removals) {
        std::size_t listeners{};
        std::size_t sources{};
        const auto count = [&](const AgentRuntimeState &agent) {
            if (agent.retired || !agent.perceptionEndpoints)
                return;
            listeners += !agent.perceptionEndpoints->listeners.empty();
            sources += !agent.perceptionEndpoints->sources.empty();
        };
        for (std::size_t index = 0; index < residents.size(); ++index)
            if (!std::ranges::binary_search(removals, index))
                count(residents[index]);
        for (const auto &agent : additions)
            count(agent);
        if (listeners > PerceptionSpatialLimits::Listeners || sources > PerceptionSpatialLimits::Sources)
            return Result<void>::Failure(MakeError(AIErrors::PerceptionSpatialLimitExceeded));
        return Result<void>::Success();
    }

    /** @copydoc AiSceneRuntime::PublishPerceptionAtSafePoint */
    Result<std::shared_ptr<const PerceptionSpatialSnapshot>> AiSceneRuntime::PublishPerceptionAtSafePoint(
        const Runtime::RuntimeSceneView &scene, const std::uint64_t revision, const std::span<const AiScenePerceptionPosition> positions,
        PerceptionSpatialBroadphase &spatial, const PerceptionFilteringPolicy &filtering) const {
        using Publication = Result<std::shared_ptr<const PerceptionSpatialSnapshot>>;
        if (auto owner = CapabilityAtSafePoint().ValidateAtSafePoint(); owner.HasError())
            return Publication::Failure(owner.ErrorValue());
        if (!scene.IsCurrent() || scene.RuntimeId() != active_->binding.scene)
            return Publication::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        auto prepared = Detail::PerceptionGroupPublication::PrepareScene(spatial, scene, revision, positions, filtering, active_->agents);
        if (prepared.HasError())
            return Publication::Failure(prepared.ErrorValue());
        auto candidate = std::move(prepared).Value();
        candidate.Publish();
        return Publication::Success(spatial.Current());
    }

    /** @copydoc AiSceneRuntime::PollPerceptionAtSensePoll */
    Result<void> AiSceneRuntime::PollPerceptionAtSensePoll(const Runtime::RuntimeSceneView &scene,
                                                           const PerceptionSpatialBroadphase &spatial, const std::uint64_t tick,
                                                           const Duration fixedStep) {
        if (auto owner = CapabilityAtSafePoint().ValidateAtSafePoint(); owner.HasError())
            return owner;
        const auto &policy = active_->perceptionExecution;
        if (!policy || !policy->generation->CanAdmit())
            return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        if (!scene.IsCurrent() || active_->binding.scene != scene.RuntimeId() || active_->perceptionSpatial != &spatial)
            return Result<void>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        if (tick == 0 || tick < active_->lastPerceptionTick || fixedStep.ToNanoseconds() != policy->memory.fixedStep.count())
            return Result<void>::Failure(MakeError(AIErrors::PerceptionMemoryInvalid));
        if (tick == active_->lastPerceptionTick)
            return active_->perceptionPollFailure ? Result<void>::Failure(*active_->perceptionPollFailure) : Result<void>::Success();
        active_->lastPerceptionTick = tick;
        active_->perceptionPollFailure.reset();
        AiWorkBudget global{policy->workUnitsPerTick};
        for (auto &agent : active_->agents) {
            if (!agent.sensing)
                continue;
            auto &sensing = *agent.sensing;
            AiWorkBudget budget{sensing.workUnits};
            for (auto &sense : sensing.senses) {
                if (const auto polled =
                        RunPerceptionSenseAtSensePoll(scene, spatial, *sense.instance, sense.query, tick, sensing.memory, budget, global);
                    polled.HasError()) {
                    active_->perceptionPollFailure = polled.ErrorValue();
                    return Result<void>::Failure(*active_->perceptionPollFailure);
                }
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc AiSceneRuntime::ReadPerceptionAtSafePoint */
    Result<PerceptionMemorySnapshot> AiSceneRuntime::ReadPerceptionAtSafePoint(const Runtime::RuntimeSceneView &scene,
                                                                               const Runtime::EntityRef recipient) {
        using Snapshot = Result<PerceptionMemorySnapshot>;
        if (auto owner = CapabilityAtSafePoint().ValidateAtSafePoint(); owner.HasError())
            return Snapshot::Failure(owner.ErrorValue());
        if (!scene.IsCurrent() || scene.RuntimeId() != active_->binding.scene || !scene.Get(recipient).HasValue())
            return Snapshot::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        if (active_->perceptionPollFailure)
            return Snapshot::Failure(*active_->perceptionPollFailure);
        Detail::SceneAgentPerception *sensing = nullptr;
        const auto [first, last] = active_->agentsByOwner.equal_range(recipient);
        for (auto owner = first; owner != last; ++owner) {
            const auto &agent = active_->agents[owner->second];
            if (!agent.sensing)
                continue;
            if (sensing)
                return Snapshot::Failure(MakeError(AIErrors::HandleInvalid));
            sensing = agent.sensing.get();
        }
        if (!sensing)
            return Snapshot::Failure(MakeError(AIErrors::RuntimeUnavailable));
        auto current = scene;
        return sensing->memory.Snapshot(active_->lastPerceptionTick, {&current, ViewSourceAlive});
    }

    /** @copydoc ProjectPerceptionSource */
    PerceptionSourceRef ProjectPerceptionSource(const Runtime::EntityRef entity) noexcept {
        return {.sceneIncarnation = entity.runtime.value, .slot = entity.entity.index, .generation = entity.entity.generation};
    }

    /** @copydoc PerceptionSceneLiveness */
    PerceptionSourceLiveness PerceptionSceneLiveness(Runtime::RuntimeScene &scene) noexcept {
        return {.context = &scene, .isAlive = SceneSourceAlive};
    }

    /** @copydoc PerceptionBlackboardProjection::Validate */
    Result<std::size_t> PerceptionBlackboardProjection::Validate(const BlackboardSchema &schema) const {
        if (!sources.IsValid() || !positions.IsValid() || sources == positions)
            return Result<std::size_t>::Failure(MakeError(AIErrors::BlackboardBatchInvalid));
        const auto sourceCapacity = CollectionCapacity(schema, sources, BlackboardValueKind::EntityReference);
        if (sourceCapacity.HasError())
            return sourceCapacity;
        const auto positionCapacity = CollectionCapacity(schema, positions, BlackboardValueKind::WorldCoordinate);
        if (positionCapacity.HasError())
            return positionCapacity;
        return Result<std::size_t>::Success(std::min(sourceCapacity.Value(), positionCapacity.Value()));
    }

    /** @copydoc PerceptionPublication::PrepareAtSafePoint */
    Result<PerceptionPublication> PerceptionPublication::PrepareAtSafePoint(const Runtime::RuntimeSceneView &scene,
                                                                            const Runtime::EntityRef listener,
                                                                            const BlackboardInstance &blackboard,
                                                                            const PerceptionBlackboardProjection projection) {
        if (!scene.IsCurrent() || scene.Get(listener).HasError() || !blackboard.IsActive())
            return Result<PerceptionPublication>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        auto schema = blackboard.SchemaAtSafePoint();
        const auto capacity = projection.Validate(*schema);
        if (capacity.HasError())
            return Result<PerceptionPublication>::Failure(capacity.ErrorValue());
        return Result<PerceptionPublication>::Success({listener, blackboard.Binding(), std::move(schema), projection, capacity.Value()});
    }

    /** @copydoc PerceptionPublication::PerceptionPublication */
    PerceptionPublication::PerceptionPublication(const Runtime::EntityRef listener, const BlackboardInstanceBinding binding,
                                                 std::shared_ptr<const BlackboardSchema> schema,
                                                 const PerceptionBlackboardProjection projection, const std::size_t capacity) noexcept
        : listener_(listener), binding_(binding), schema_(std::move(schema)), projection_(projection), capacity_(capacity) {}

    /** @copydoc PerceptionPublication::MatchesBinding */
    bool PerceptionPublication::MatchesBinding(const Runtime::RuntimeSceneView &scene, const BlackboardInstance &blackboard) const {
        return scene.IsCurrent() && scene.RuntimeId() == listener_.runtime && scene.Get(listener_).HasValue() && blackboard.IsActive() &&
               blackboard.Binding() == binding_ && blackboard.SchemaAtSafePoint() == schema_;
    }

    /** @copydoc PerceptionPublication::PublishAtBlackboardSync */
    Result<PerceptionPublicationSnapshot> PerceptionPublication::PublishAtBlackboardSync(Runtime::RuntimeScene &scene,
                                                                                         const PerceptionSpatialBroadphase &filtering,
                                                                                         AIPerceptionMemory &memory,
                                                                                         BlackboardInstance &blackboard,
                                                                                         const std::uint64_t simulationTick) {
        using PublicationResult = Result<PerceptionPublicationSnapshot>;
        const auto spatial = filtering.Current();
        const auto view = scene.View();
        if (!spatial || spatial->Scene() != view.RuntimeId() || !MatchesBinding(view, blackboard) ||
            memory.SceneIncarnation() != view.RuntimeId().value || binding_.agent != memory.Agent())
            return PublicationResult::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        if (current_ && (simulationTick < current_->simulationTick || current_->revision == std::numeric_limits<std::uint64_t>::max()))
            return PublicationResult::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
        if (current_ && filtering_ == spatial && simulationTick == current_->simulationTick)
            return Current(view, filtering, blackboard);
        auto createdBatch = blackboard.BeginWriteBatch();
        if (createdBatch.HasError())
            return PublicationResult::Failure(createdBatch.ErrorValue());
        const auto remembered = memory.Snapshot(simulationTick, PerceptionSceneLiveness(scene));
        if (remembered.HasError())
            return PublicationResult::Failure(remembered.ErrorValue());
        const PerceptionPublicationSnapshot identity{.agent = memory.Agent(),
                                                     .listener = listener_,
                                                     .revision = current_ ? current_->revision + 1 : 1,
                                                     .spatialRevision = spatial->Revision(),
                                                     .simulationTick = simulationTick};
        auto next = BuildProjection(view, *spatial, remembered.Value(), identity, capacity_);
        if (next.admittedCount > capacity_) {
            rejectedByCapacity_ = next.admittedCount;
            return PublicationResult::Failure(MakeError(AIErrors::PerceptionDescriptorLimitExceeded));
        }
        auto batch = std::move(createdBatch).Value();
        if (const auto staged = next.Stage(batch, projection_, spatial->ReadAuthorization()); staged.HasError())
            return PublicationResult::Failure(staged.ErrorValue());
        const auto committed = blackboard.CommitAtBlackboardSync(std::move(batch));
        if (committed.HasError())
            return PublicationResult::Failure(committed.ErrorValue());
        next.snapshot.blackboardRevision = committed.Value().revision;
        filtering_ = spatial;
        current_ = next.snapshot;
        rejectedByCapacity_ = 0;
        return PublicationResult::Success(next.snapshot);
    }

    /** @copydoc PerceptionPublication::Current */
    Result<PerceptionPublicationSnapshot> PerceptionPublication::Current(const Runtime::RuntimeSceneView &scene,
                                                                         const PerceptionSpatialBroadphase &filtering,
                                                                         const BlackboardInstance &blackboard) const {
        if (!current_ || filtering.Current() != filtering_ || !MatchesBinding(scene, blackboard))
            return Result<PerceptionPublicationSnapshot>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        if (blackboard.RevisionAtSafePoint() != current_->blackboardRevision)
            return Result<PerceptionPublicationSnapshot>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        for (std::size_t index = 0; index < current_->memory.count; ++index) {
            const auto &record = current_->memory.entries[index];
            if (scene.Get({scene.RuntimeId(), {record.key.source.slot, record.key.source.generation}}).HasError() ||
                scene.Get({scene.RuntimeId(), {record.provenance.slot, record.provenance.generation}}).HasError())
                return Result<PerceptionPublicationSnapshot>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        }
        return Result<PerceptionPublicationSnapshot>::Success(*current_);
    }

    /** @copydoc PerceptionPublication::Reset */
    void PerceptionPublication::Reset() noexcept {
        current_.reset();
        filtering_.reset();
        rejectedByCapacity_ = 0;
    }
}  // namespace Horo::AI
