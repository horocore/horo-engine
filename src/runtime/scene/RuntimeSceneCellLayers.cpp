#include "Horo/Runtime/Scene/RuntimeSceneCellLayers.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace Horo::Runtime {
    namespace {
        namespace W = WorldStreaming;

        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        /** @brief Validates count/storage ceilings without overflowing or consuming the baseline. */
        Result<std::size_t> RetainedCharge(const RuntimeSceneCellPayload &baseline, const std::size_t layers, const std::size_t memberships,
                                           const SceneCellLayerLimits limits) {
            if (limits.maximumLayers == 0 || limits.maximumMemberships == 0 || limits.maximumRetainedBytes == 0)
                return Failure<std::size_t>(SceneCellPayloadErrors::Invalid);
            std::size_t bytes = baseline.RetainedBytes();
            if (layers > limits.maximumLayers || memberships > limits.maximumMemberships || bytes > limits.maximumRetainedBytes)
                return Failure<std::size_t>(SceneCellPayloadErrors::CapacityExceeded);
            if (layers > (limits.maximumRetainedBytes - bytes) / sizeof(SceneCellDataLayer))
                return Failure<std::size_t>(SceneCellPayloadErrors::CapacityExceeded);
            bytes += layers * sizeof(SceneCellDataLayer);
            if (memberships > (limits.maximumRetainedBytes - bytes) / sizeof(SceneCellLayerMembership))
                return Failure<std::size_t>(SceneCellPayloadErrors::CapacityExceeded);
            return Result<std::size_t>::Success(bytes + memberships * sizeof(SceneCellLayerMembership));
        }

        /** @brief Checks exact cell identity and canonical manifest-backed layer references. */
        Result<void> ValidateLayers(const W::WorldPartitionDescriptor &partition, const RuntimeSceneCellPayload &baseline,
                                    const SceneCellPayloadIdentity &expected, const std::span<const SceneCellDataLayer> layers,
                                    const CancellationToken &cancellation) {
            if (baseline.Identity() != expected || expected.partition != partition.Partition())
                return Failure<void>(SceneCellPayloadErrors::Stale);
            if (!expected.scene.IsValid() || expected.revision.value == 0 || std::ranges::none_of(partition.Cells(), [&](const auto &cell) {
                return cell.id == expected.cell;
            }))
                return Failure<void>(SceneCellPayloadErrors::Invalid);
            for (std::size_t index = 0; index < layers.size(); ++index) {
                if (cancellation.IsCancellationRequested())
                    return Failure<void>(SceneCellPayloadErrors::Cancelled);
                const auto &layer = layers[index];
                if (!layer.layer.IsValid() || !layer.ownershipRevision.IsValid() || (index != 0 && layers[index - 1].layer >= layer.layer))
                    return Failure<void>(SceneCellPayloadErrors::Invalid);
                const auto found = std::ranges::lower_bound(partition.Layers(), layer.layer, {}, &W::WorldLayerDescriptor::id);
                if (found == partition.Layers().end() || found->id != layer.layer)
                    return Failure<void>(SceneCellPayloadErrors::Invalid);
                if (found->flags != layer.flags)
                    return Failure<void>(SceneCellPayloadErrors::Stale);
            }
            return Result<void>::Success();
        }

        /** @brief Validates references once using bounded entity scratch and canonical edge lookup. */
        Result<void> ValidateMemberships(const RuntimeSceneCellPayload &baseline, const std::span<const SceneCellDataLayer> layers,
                                         const std::span<const SceneCellLayerMembership> memberships,
                                         const CancellationToken &cancellation) {
            std::unordered_set<std::uint64_t> objects;
            for (const auto &entity : baseline.Definition().Entities())
                objects.insert(entity.object.value);
            for (std::size_t index = 0; index < memberships.size(); ++index) {
                if (cancellation.IsCancellationRequested())
                    return Failure<void>(SceneCellPayloadErrors::Cancelled);
                const auto &edge = memberships[index];
                if (!objects.contains(edge.object.value) || (index != 0 && memberships[index - 1] >= edge))
                    return Failure<void>(SceneCellPayloadErrors::Invalid);
                const auto found = std::ranges::lower_bound(layers, edge.layer, {}, &SceneCellDataLayer::layer);
                if (found == layers.end() || found->layer != edge.layer)
                    return Failure<void>(SceneCellPayloadErrors::Invalid);
            }
            return Result<void>::Success();
        }

        /** @brief Checks complete live ownership/state snapshots against every encoded classification revision. */
        Result<void> ValidateStates(const RuntimeSceneCellLayers &payload, const std::span<const W::WorldLayerFilterCandidate> candidates,
                                    const std::span<const W::WorldLayerStateRecord> states) {
            if (candidates.size() != payload.Layers().size() || states.size() != candidates.size())
                return Failure<void>(SceneCellPayloadErrors::Invalid);
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                const auto &encoded = payload.Layers()[index];
                const auto &candidate = candidates[index];
                if (!states[index].IsValid())
                    return Failure<void>(SceneCellPayloadErrors::Invalid);
                if (candidate.ownership.layer != encoded.layer || candidate.ownership.revision != encoded.ownershipRevision ||
                    candidate.flags != encoded.flags || states[index].ownership != candidate.ownership)
                    return Failure<void>(SceneCellPayloadErrors::Stale);
            }
            return Result<void>::Success();
        }

        /** @brief Evaluates union membership without copying payloads or allocating per entity. */
        bool Included(const SceneObjectId object, const RuntimeSceneCellLayers &payload,
                      const std::span<const W::WorldLayerFilterDecision> decisions,
                      const std::span<const W::WorldLayerStateRecord> states) {
            const auto edges = payload.Memberships();
            auto edge = std::ranges::lower_bound(edges, object, {}, &SceneCellLayerMembership::object);
            if (edge == edges.end() || edge->object != object)
                return true;
            for (; edge != edges.end() && edge->object == object; ++edge) {
                const auto layer = std::ranges::lower_bound(payload.Layers(), edge->layer, {}, &SceneCellDataLayer::layer);
                const auto index = static_cast<std::size_t>(layer - payload.Layers().begin());
                if (decisions[index].IsIncluded() && states[index].state == W::WorldLayerState::Activated)
                    return true;
            }
            return false;
        }

        /** @brief Materializes the one selected candidate through authoritative Scene validation. */
        Result<RuntimeSceneDefinition> SelectDefinition(const RuntimeSceneCellLayers &payload,
                                                        const std::span<const W::WorldLayerFilterDecision> decisions,
                                                        const std::span<const W::WorldLayerStateRecord> states,
                                                        const CancellationToken &cancellation) {
            const auto &baseline = payload.Baseline().Definition();
            SceneDefinitionBuilder builder{baseline.Id(), baseline.Revision()};
            for (const auto &entity : baseline.Entities()) {
                if (cancellation.IsCancellationRequested())
                    return Failure<RuntimeSceneDefinition>(SceneCellPayloadErrors::Cancelled);
                if (Included(entity.object, payload, decisions, states))
                    builder.Add(entity);
            }
            for (const auto &dependency : baseline.AssetDependencies()) {
                if (cancellation.IsCancellationRequested())
                    return Failure<RuntimeSceneDefinition>(SceneCellPayloadErrors::Cancelled);
                if (const auto required = builder.RequireAsset(dependency); required.HasError())
                    return Result<RuntimeSceneDefinition>::Failure(required.ErrorValue());
            }
            return std::move(builder).Build();
        }

        /** @brief Retains all state fences and the host lease until queued work retires. */
        class LayerPublicationCheck final : public ScenePublicationCheck {
        public:
            LayerPublicationCheck(SceneCellLayerSelectionEvidence evidence, W::StreamingFence fence,
                                  std::shared_ptr<const SceneCellLayerAuthority> authority, CancellationToken cancellation)
                : evidence_(std::move(evidence)), fence_(std::move(fence)), authority_(std::move(authority)),
                  cancellation_(std::move(cancellation)) {}

            Result<void> ValidatePublication() const override {
                if (cancellation_.IsCancellationRequested())
                    return Failure<void>(SceneCellPayloadErrors::Cancelled);
                return authority_->ValidatePublication(evidence_, fence_);
            }

        private:
            SceneCellLayerSelectionEvidence evidence_;
            W::StreamingFence fence_;
            std::shared_ptr<const SceneCellLayerAuthority> authority_;
            CancellationToken cancellation_;
        };
    }  // namespace

    /** @copydoc RuntimeSceneCellLayers::RuntimeSceneCellLayers */
    RuntimeSceneCellLayers::RuntimeSceneCellLayers(RuntimeSceneCellPayload &&baseline, std::vector<SceneCellDataLayer> layers,
                                                   std::vector<SceneCellLayerMembership> memberships,
                                                   const std::size_t retainedBytes) noexcept
        : baseline_(std::move(baseline)), layers_(std::move(layers)), memberships_(std::move(memberships)), retainedBytes_(retainedBytes) {}

    /** @copydoc RuntimeSceneCellLayers::RuntimeSceneCellLayers */
    RuntimeSceneCellLayers::RuntimeSceneCellLayers(RuntimeSceneCellLayers &&other) noexcept
        : baseline_(std::move(other.baseline_)), layers_(std::move(other.layers_)), memberships_(std::move(other.memberships_)),
          retainedBytes_(other.retainedBytes_), usable_(std::exchange(other.usable_, false)) {}

    /** @copydoc RuntimeSceneCellLayers::Baseline */
    const RuntimeSceneCellPayload &RuntimeSceneCellLayers::Baseline() const noexcept {
        return baseline_;
    }

    /** @copydoc RuntimeSceneCellLayers::Layers */
    std::span<const SceneCellDataLayer> RuntimeSceneCellLayers::Layers() const noexcept {
        return layers_;
    }

    /** @copydoc RuntimeSceneCellLayers::Memberships */
    std::span<const SceneCellLayerMembership> RuntimeSceneCellLayers::Memberships() const noexcept {
        return memberships_;
    }

    /** @copydoc RuntimeSceneCellLayers::RetainedBytes */
    std::size_t RuntimeSceneCellLayers::RetainedBytes() const noexcept {
        return retainedBytes_;
    }

    /** @copydoc RuntimeSceneCellLayers::IsUsable */
    bool RuntimeSceneCellLayers::IsUsable() const noexcept {
        return usable_;
    }

    /** @copydoc EncodeRuntimeSceneCellLayers */
    Result<RuntimeSceneCellLayers> EncodeRuntimeSceneCellLayers(const W::WorldPartitionDescriptor &partition,
                                                                RuntimeSceneCellPayload &&baseline,
                                                                const SceneCellPayloadIdentity &expected,
                                                                const std::span<const SceneCellDataLayer> layers,
                                                                const std::span<const SceneCellLayerMembership> memberships,
                                                                const SceneCellLayerLimits limits, const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Failure<RuntimeSceneCellLayers>(SceneCellPayloadErrors::Cancelled);
        const auto charge = RetainedCharge(baseline, layers.size(), memberships.size(), limits);
        if (charge.HasError())
            return Result<RuntimeSceneCellLayers>::Failure(charge.ErrorValue());
        if (const auto valid = ValidateLayers(partition, baseline, expected, layers, cancellation); valid.HasError())
            return Result<RuntimeSceneCellLayers>::Failure(valid.ErrorValue());
        if (const auto valid = ValidateMemberships(baseline, layers, memberships, cancellation); valid.HasError())
            return Result<RuntimeSceneCellLayers>::Failure(valid.ErrorValue());
        std::vector<SceneCellDataLayer> ownedLayers;
        ownedLayers.reserve(layers.size());
        for (const auto &layer : layers) {
            if (cancellation.IsCancellationRequested())
                return Failure<RuntimeSceneCellLayers>(SceneCellPayloadErrors::Cancelled);
            ownedLayers.push_back(layer);
        }
        std::vector<SceneCellLayerMembership> ownedMemberships;
        ownedMemberships.reserve(memberships.size());
        for (const auto &edge : memberships) {
            if (cancellation.IsCancellationRequested())
                return Failure<RuntimeSceneCellLayers>(SceneCellPayloadErrors::Cancelled);
            ownedMemberships.push_back(edge);
        }
        if (cancellation.IsCancellationRequested())
            return Failure<RuntimeSceneCellLayers>(SceneCellPayloadErrors::Cancelled);
        return Result<RuntimeSceneCellLayers>::Success(
            RuntimeSceneCellLayers{std::move(baseline), std::move(ownedLayers), std::move(ownedMemberships), charge.Value()});
    }

    /** @copydoc RuntimeSceneCellLayerSelection::RuntimeSceneCellLayerSelection */
    RuntimeSceneCellLayerSelection::RuntimeSceneCellLayerSelection(RuntimeSceneDefinition definition,
                                                                   SceneCellLayerSelectionEvidence evidence) noexcept
        : definition_(std::move(definition)), evidence_(std::move(evidence)) {}

    /** @copydoc RuntimeSceneCellLayerSelection::RuntimeSceneCellLayerSelection */
    RuntimeSceneCellLayerSelection::RuntimeSceneCellLayerSelection(RuntimeSceneCellLayerSelection &&other) noexcept
        : definition_(std::move(other.definition_)), evidence_(std::move(other.evidence_)), usable_(std::exchange(other.usable_, false)) {}

    /** @copydoc RuntimeSceneCellLayerSelection::IsUsable */
    bool RuntimeSceneCellLayerSelection::IsUsable() const noexcept {
        return usable_;
    }

    /** @copydoc RuntimeSceneCellLayerSelection::Definition */
    const RuntimeSceneDefinition &RuntimeSceneCellLayerSelection::Definition() const noexcept {
        return definition_;
    }

    /** @copydoc RuntimeSceneCellLayerSelection::Evidence */
    const SceneCellLayerSelectionEvidence &RuntimeSceneCellLayerSelection::Evidence() const noexcept {
        return evidence_;
    }

    /** @copydoc FilterRuntimeSceneCellLayers */
    Result<RuntimeSceneCellLayerSelection> FilterRuntimeSceneCellLayers(const RuntimeSceneCellLayers &payload,
                                                                        const W::WorldLayerFilterPolicy &policy,
                                                                        const std::span<const W::WorldLayerFilterCandidate> candidates,
                                                                        const std::span<const W::WorldLayerStateRecord> states,
                                                                        const W::WorldLayerFilterContext &context,
                                                                        const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Failure<RuntimeSceneCellLayerSelection>(SceneCellPayloadErrors::Cancelled);
        if (!payload.IsUsable())
            return Failure<RuntimeSceneCellLayerSelection>(SceneCellPayloadErrors::Invalid);
        if (payload.Baseline().Identity().partition != context.expectedWorld.partition)
            return Failure<RuntimeSceneCellLayerSelection>(SceneCellPayloadErrors::Stale);
        if (candidates.size() > context.maximumCandidates)
            return Failure<RuntimeSceneCellLayerSelection>(SceneCellPayloadErrors::CapacityExceeded);
        if (const auto valid = ValidateStates(payload, candidates, states); valid.HasError())
            return Result<RuntimeSceneCellLayerSelection>::Failure(valid.ErrorValue());
        std::vector<W::WorldLayerFilterDecision> decisions(candidates.size());
        const auto filtered = W::FilterWorldLayers(policy, candidates, context, decisions);
        if (filtered.HasError())
            return Result<RuntimeSceneCellLayerSelection>::Failure(filtered.ErrorValue());
        auto definition = SelectDefinition(payload, decisions, states, cancellation);
        if (definition.HasError())
            return Result<RuntimeSceneCellLayerSelection>::Failure(definition.ErrorValue());
        SceneCellLayerSelectionEvidence evidence{payload.Baseline().Identity(), context.expectedWorld, policy, {}};
        evidence.states.reserve(states.size());
        for (const auto &state : states)
            evidence.states.push_back(state.Fence());
        if (cancellation.IsCancellationRequested())
            return Failure<RuntimeSceneCellLayerSelection>(SceneCellPayloadErrors::Cancelled);
        return Result<RuntimeSceneCellLayerSelection>::Success(
            RuntimeSceneCellLayerSelection{std::move(definition).Value(), std::move(evidence)});
    }

    /** @copydoc QueueRuntimeSceneCellLayers */
    Result<void> QueueRuntimeSceneCellLayers(RuntimeSceneService &service, const RuntimeSceneCellLayerSelection &selection,
                                             const W::StreamingFence &fence, std::shared_ptr<const SceneCellLayerAuthority> authority,
                                             const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Failure<void>(SceneCellPayloadErrors::Cancelled);
        if (!selection.IsUsable() || !authority || !fence.IsValid())
            return Failure<void>(SceneCellPayloadErrors::Invalid);
        const auto &evidence = selection.Evidence();
        if (fence.partition != evidence.identity.partition || fence.cell != evidence.identity.cell || fence.epoch != evidence.world.epoch)
            return Failure<void>(SceneCellPayloadErrors::Stale);
        return service.QueuePreparationWithPublicationCheck(selection.Definition(),
                                                            std::make_unique<LayerPublicationCheck>(evidence, fence, std::move(authority),
                                                                                                    cancellation));
    }
}  // namespace Horo::Runtime
