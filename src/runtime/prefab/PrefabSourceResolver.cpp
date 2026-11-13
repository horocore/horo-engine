#include "Horo/Prefab/PrefabSourceResolver.h"

#include "Horo/Prefab/PrefabDiagnostics.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabSourceExpansionInternal.h"
#include "PrefabValueOverridesInternal.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace Horo::Prefab {
    namespace {
        /** @brief Captures authoritative canonical source evidence once at immutable
         * resolver admission. */
        Result<std::vector<PrefabCanonicalSourceCommitment>> CaptureCanonicalSources(
            const std::span<const PrefabDependencySource> sources) {
            using Output = std::vector<PrefabCanonicalSourceCommitment>;
            Output commitments;
            commitments.reserve(sources.size());
            for (const auto &source : sources) {
                const auto bytes = source.document.SerializeCanonical();
                if (bytes.HasError())
                    return Result<Output>::Failure(bytes.ErrorValue());
                if (bytes.Value().size() > PrefabHardLimits::SourceDocumentBytes)
                    return Result<Output>::Failure(MakeError(PrefabErrors::PayloadTooLarge));
                commitments.emplace_back(source.document.Data().assetId, ComputeSha256(std::as_bytes(std::span{bytes.Value()})),
                                         bytes.Value().size());
            }
            return Result<Output>::Success(std::move(commitments));
        }

        /** @brief Finds an owned source by stable asset identity. */
        [[nodiscard]] const PrefabDependencySource *FindSource(const std::span<const PrefabDependencySource> sources,
                                                               const Assets::AssetId assetId) noexcept {
            const auto found = std::ranges::lower_bound(sources, assetId, {}, [](const PrefabDependencySource &source) {
                return source.document.Data().assetId;
            });
            return found != sources.end() && found->document.Data().assetId == assetId ? std::to_address(found) : nullptr;
        }

        template <typename ValueT> class PopBackGuard final {
        public:
            explicit PopBackGuard(std::vector<ValueT> &values) noexcept : values_(values) {}

            ~PopBackGuard() noexcept {
                values_.pop_back();
            }

            PopBackGuard(const PopBackGuard &) = delete;
            PopBackGuard &operator=(const PopBackGuard &) = delete;

        private:
            std::vector<ValueT> &values_;
        };

        using ExpansionState = Detail::SourceExpansionState;

        /** @brief Records the complete source path that made a required expansion fail.
         */
        [[nodiscard]] Result<void> Fail(ExpansionState &state, Error error, const std::optional<Assets::AssetId> next = std::nullopt) {
            state.failureChain = state.active;
            if (next)
                state.failureChain.push_back(*next);
            return Result<void>::Failure(std::move(error));
        }

        /** @brief Adds immutable operation context without changing the underlying
         * typed failure. */
        void AddFailureContext(Error &error, const PrefabInstanceId instance, const std::span<const Assets::AssetId> sourceChain) {
            std::string context = std::format("Prefab instance {} source chain", instance.Value());
            for (const Assets::AssetId source : sourceChain)
                context += " -> " + source.ToString();
            if (sourceChain.size() > 2 && context.size() + 2 + error.message.size() > MaximumPrefabDiagnosticMessageBytes) {
                context = std::format("Prefab instance {} source chain ({} assets, intermediate assets omitted) {} -> {}", instance.Value(),
                                      sourceChain.size(), sourceChain.front().ToString(), sourceChain.back().ToString());
            }
            error.message = context + ": " + error.message;
        }

        [[nodiscard]] Result<void> ExpandSource(ExpansionState &state, const PrefabDependencySource &source, std::size_t nestedDepth,
                                                std::size_t variantDepth);

        /** @brief Resolves the complete immediate parent before applying this variant
         * layer, with no outer placement mount. */
        Result<void> MaterializeVariantParent(ExpansionState &state, const PrefabDependencySource &source, std::size_t nestedDepth,
                                              std::size_t variantDepth) {
            if (variantDepth >= state.limits.Policy().maximumVariantInheritanceDepth)
                return Fail(state, MakeError(PrefabErrors::HierarchyDepthExceeded));
            const auto &document = source.document.Data();
            const auto asset = document.composition->variantParent->Asset();
            const auto *parent = FindSource(state.sources, asset);
            if (!parent)
                return Fail(state, MakeError(PrefabErrors::DependencyUnavailable), asset);
            state.originChain.push_back({PrefabCompositionOrigin::Variant, document.assetId, asset, {}});
            const PopBackGuard guard{state.originChain};
            const auto begin = state.objects.size();
            if (auto expanded = ExpandSource(state, *parent, nestedDepth, variantDepth + 1); expanded.HasError())
                return expanded;
            return Detail::ApplySourceOverrideLayer(state, document.composition->variantOverrides, *parent, document.assetId, {}, begin);
        }

        /** @brief Admits one object before identity allocation or output mutation. */
        Result<void> AdmitLocalObject(ExpansionState &state) {
            if (state.cancellation.IsCancellationRequested())
                return Fail(state, MakeError(PrefabErrors::Cancelled));
            if (state.objects.size() >= state.limits.Policy().maximumObjectCount)
                return Fail(state, MakeError(PrefabErrors::ObjectCountExceeded));
            if (const auto charged = state.budget.Consume(1); charged.HasError())
                return Fail(state, charged.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Copies concrete source locals without importing an outer placement
         * transform before its overrides. */
        Result<void> MaterializeLocalObjects(ExpansionState &state, const PrefabDocumentData &document) {
            for (const auto &object : document.objects) {
                if (auto admitted = AdmitLocalObject(state); admitted.HasError())
                    return admitted;
                if (auto charged = state.budget.Consume(state.originChain.size()); charged.HasError())
                    return Fail(state, charged.ErrorValue());
                auto address = PrefabObjectAddress::Create(state.scope, object.localId);
                if (address.HasError())
                    return Fail(state, address.ErrorValue());
                const ExpandedPrefabObjectKey key{state.instance, address.Value()};
                std::optional<ExpandedPrefabObjectKey> parent;
                if (object.parentLocalId) {
                    auto target = PrefabObjectAddress::Create(state.scope, *object.parentLocalId);
                    if (target.HasError())
                        return Fail(state, target.ErrorValue());
                    parent = ExpandedPrefabObjectKey{state.instance, target.Value()};
                }
                state.objects.emplace_back(document.assetId, key, parent, object, object.localTransform, Math::Transform{},
                                           state.originChain);
            }
            return Result<void>::Success();
        }

        /** @brief Composes only the complete post-override root with its one actual
         * containing placement. */
        Result<void> MountNested(ExpansionState &state, std::size_t begin, const std::optional<ExpandedPrefabObjectKey> &parent,
                                 const Math::Transform &transform) {
            if (state.unresolved)
                return Result<void>::Success();
            auto &root = state.objects[begin];
            const auto composed = ValueDetail::ComposePrefabPlacement(transform, root.object.localTransform);
            if (composed.HasError())
                return Fail(state, composed.ErrorValue());
            root.parent = parent;
            root.placementTransform = transform;
            root.effectiveLocalTransform = composed.Value();
            return Result<void>::Success();
        }

        /** @brief Fully resolves and applies one stable nested occurrence before its
         * scope and mount guards retire. */
        Result<void> MaterializeNestedPlacement(ExpansionState &state, const PrefabDocumentData &document,
                                                const NestedPrefabPlacement &placement, std::size_t nestedDepth, std::size_t variantDepth,
                                                const std::optional<ExpandedPrefabObjectKey> &sourceRoot) {
            if (state.cancellation.IsCancellationRequested())
                return Fail(state, MakeError(PrefabErrors::Cancelled));
            if (nestedDepth >= state.limits.Policy().maximumNestedPrefabDepth)
                return Fail(state, MakeError(PrefabErrors::HierarchyDepthExceeded));
            const auto asset = placement.sourcePrefab.Asset();
            const auto *nested = FindSource(state.sources, asset);
            if (!nested)
                return Fail(state, MakeError(PrefabErrors::DependencyUnavailable), asset);
            auto mountParent = sourceRoot;
            if (placement.parentLocalId) {
                auto parent = PrefabObjectAddress::Create(state.scope, *placement.parentLocalId);
                if (parent.HasError())
                    return Fail(state, parent.ErrorValue());
                mountParent = ExpandedPrefabObjectKey{state.instance, parent.Value()};
            }
            state.scope.push_back(placement.placementLocalId);
            const PopBackGuard scopeGuard{state.scope};
            state.originChain.emplace_back(PrefabCompositionOrigin::Nested, document.assetId, asset, placement.placementLocalId);
            const PopBackGuard originGuard{state.originChain};
            const auto begin = state.objects.size();
            if (auto expanded = ExpandSource(state, *nested, nestedDepth + 1, variantDepth); expanded.HasError())
                return expanded;
            if (auto applied = Detail::ApplySourceOverrideLayer(state, placement.valueOverrides, *nested, document.assetId,
                                                                placement.placementLocalId, begin);
                applied.HasError())
                return applied;
            return MountNested(state, begin, mountParent, placement.localRootTransform);
        }

        /** @brief Processes complete nested occurrences in canonical placement order
         * without continuing past unresolved input. */
        Result<void> MaterializeNestedPlacements(ExpansionState &state, const PrefabDocumentData &document, std::size_t nestedDepth,
                                                 std::size_t variantDepth, const std::optional<ExpandedPrefabObjectKey> &sourceRoot) {
            if (!document.composition)
                return Result<void>::Success();
            for (const auto &placement : document.composition->nestedPlacements) {
                if (auto result = MaterializeNestedPlacement(state, document, placement, nestedDepth, variantDepth, sourceRoot);
                    result.HasError())
                    return result;
                if (state.unresolved)
                    break;
            }
            return Result<void>::Success();
        }

        /** @brief Recursively materializes one source using only immutable
         * resolver-owned documents. */
        [[nodiscard]] Result<void> ExpandSource(ExpansionState &state, const PrefabDependencySource &source, const std::size_t nestedDepth,
                                                const std::size_t variantDepth) {
            if (state.cancellation.IsCancellationRequested())
                return Fail(state, MakeError(PrefabErrors::Cancelled));
            if (const Assets::AssetId sourceAsset = source.document.Data().assetId;
                std::ranges::find(state.active, sourceAsset) != state.active.end())
                return Fail(state, MakeError(PrefabErrors::DependencyGraphInvalid), sourceAsset);
            state.active.push_back(source.document.Data().assetId);
            const PopBackGuard activeGuard{state.active};

            const PrefabDocumentData &document = source.document.Data();
            if (document.composition && document.composition->variantParent)
                return MaterializeVariantParent(state, source, nestedDepth, variantDepth);
            if (auto local = MaterializeLocalObjects(state, document); local.HasError())
                return local;
            const auto rootAddress = PrefabObjectAddress::Create(state.scope, LocalObjectId{});
            if (rootAddress.HasError())
                return Fail(state, rootAddress.ErrorValue());
            return MaterializeNestedPlacements(state, document, nestedDepth, variantDepth,
                                               ExpandedPrefabObjectKey{state.instance, rootAddress.Value()});
        }

        /** @brief Adds exact stable source context to validation failure without
         * exposing partial hierarchy. */
        Result<void> ExpandAdmittedSource(ExpansionState &state, const PrefabDependencySource &root) {
            auto result = Detail::PreflightSourceExpansion(state, root);
            if (result.HasValue())
                result = ExpandSource(state, root, 0, 0);
            if (result.HasValue())
                return result;
            auto error = result.ErrorValue();
            if (state.failureChain.empty())
                state.failureChain.push_back(root.document.Data().assetId);
            AddFailureContext(error, state.instance, state.failureChain);
            return Result<void>::Failure(std::move(error));
        }

        /** @brief Registry advancement alone is harmless; all reachable semantic
         * authorities must still match exactly. */
        bool SameContents(const PrefabResolutionRevision &a, const PrefabResolutionRevision &b) noexcept {
            return a.rootSource == b.rootSource && a.dependencies == b.dependencies && a.edges == b.edges &&
                   a.canonicalSources == b.canonicalSources && a.valueSchemaRevision == b.valueSchemaRevision &&
                   a.sourceRevisionPolicy == b.sourceRevisionPolicy && a.revisionSkews == b.revisionSkews;
        }

        /** @brief Retains only revision skew evidence reachable from this root,
         * together with exact schema policy. */
        void CaptureLayerEvidence(PrefabResolutionRevision &revision, const PrefabDependencyGraphSnapshot &graph,
                                  const std::span<const Assets::AssetId> assets, const PrefabValueSchemaSnapshot *schemas) {
            revision.sourceRevisionPolicy = graph.RevisionPolicy();
            for (const auto &skew : graph.RevisionSkews())
                if (std::ranges::binary_search(assets, skew.owner))
                    revision.revisionSkews.push_back(skew);
            revision.valueSchemaRevision = schemas ? schemas->Revision() : Sha256Digest{};
        }

    }  // namespace

    /** @copydoc EffectivePrefabCandidate::EffectivePrefabCandidate */
    EffectivePrefabCandidate::EffectivePrefabCandidate(Assets::AssetId rootAsset, PrefabResolutionRevision revision,
                                                       std::vector<ResolvedPrefabObject> objects, const Sha256Digest &valueOverrides,
                                                       const PrefabMetricHandles &metrics) noexcept
        : rootAsset_(rootAsset), revision_(std::move(revision)), objects_(std::move(objects)), valueOverrides_(valueOverrides),
          metrics_(metrics) {}

    /** @copydoc EffectivePrefabCandidate::RootAsset */
    Assets::AssetId EffectivePrefabCandidate::RootAsset() const noexcept {
        return rootAsset_;
    }

    /** @copydoc EffectivePrefabCandidate::Revision */
    const PrefabResolutionRevision &EffectivePrefabCandidate::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc EffectivePrefabCandidate::ValueOverridesDigest */
    Sha256Digest EffectivePrefabCandidate::ValueOverridesDigest() const noexcept {
        return valueOverrides_;
    }

    /** @copydoc EffectivePrefabCandidate::Objects */
    std::span<const ResolvedPrefabObject> EffectivePrefabCandidate::Objects() const noexcept {
        return objects_;
    }

    /** @copydoc PrefabSourceResolverSnapshot::PrefabSourceResolverSnapshot */
    PrefabSourceResolverSnapshot::PrefabSourceResolverSnapshot(PrefabDependencyGraphSnapshot graph,
                                                               std::vector<PrefabDependencySource> sources,
                                                               std::vector<PrefabCanonicalSourceCommitment> commitments,
                                                               const PrefabMetricHandles &metrics,
                                                               std::shared_ptr<const PrefabValueSchemaSnapshot> schemas) noexcept
        : graph_(std::move(graph)), sources_(std::move(sources)), commitments_(std::move(commitments)), metrics_(metrics),
          schemas_(std::move(schemas)) {}

    /** @copydoc PrefabSourceResolverSnapshot::RegistryRevision */
    Assets::AssetRegistryRevision PrefabSourceResolverSnapshot::RegistryRevision() const noexcept {
        return graph_.RegistryRevision();
    }

    /** @copydoc PrefabSourceResolverSnapshot::DependencyGraph */
    const PrefabDependencyGraphSnapshot &PrefabSourceResolverSnapshot::DependencyGraph() const noexcept {
        return graph_;
    }

    /** @copydoc PrefabSourceResolverSnapshot::Sources */
    std::span<const PrefabDependencySource> PrefabSourceResolverSnapshot::Sources() const noexcept {
        return sources_;
    }

    /** @copydoc PrefabSourceResolverSnapshot::CanonicalSourceCommitments */
    std::span<const PrefabCanonicalSourceCommitment> PrefabSourceResolverSnapshot::CanonicalSourceCommitments() const noexcept {
        return commitments_;
    }

    /** @copydoc PrefabSourceResolverSnapshot::Resolve */
    Result<EffectivePrefabCandidate> PrefabSourceResolverSnapshot::Resolve(const Assets::AssetId rootAsset, const PrefabInstanceId instance,
                                                                           const PrefabLimitProfile &limits,
                                                                           const CancellationToken &cancellation) const {
        if (!graph_.RevisionSkews().empty())
            return Result<EffectivePrefabCandidate>::Failure(MakeError(PrefabErrors::DependencyRevisionMismatch));
        auto resolved = ResolveOverrides(rootAsset, instance, limits, cancellation);
        if (resolved.HasError())
            return Result<EffectivePrefabCandidate>::Failure(resolved.ErrorValue());
        auto value = std::move(resolved).Value();
        if (value.effective)
            return Result<EffectivePrefabCandidate>::Success(std::move(*value.effective));
        const auto &first = value.layers.back().rebased;
        const bool conflict = !first.conflicts.empty() || !first.componentConflicts.empty();
        return Result<EffectivePrefabCandidate>::Failure(
            MakeError(conflict ? PrefabErrors::OverrideConflict : PrefabErrors::OverrideOrphan));
    }

    /** @copydoc PrefabSourceResolverSnapshot::ValueSchemaRevision */
    Sha256Digest PrefabSourceResolverSnapshot::ValueSchemaRevision() const noexcept {
        return schemas_ ? schemas_->Revision() : Sha256Digest{};
    }

    /** @copydoc PrefabSourceResolverSnapshot::ValueSchemas */
    const PrefabValueSchemaSnapshot *PrefabSourceResolverSnapshot::ValueSchemas() const noexcept {
        return schemas_.get();
    }

    /** @copydoc PrefabSourceResolverSnapshot::ResolveOverrides */
    Result<PrefabSourceOverrideResolution> PrefabSourceResolverSnapshot::ResolveOverrides(const Assets::AssetId rootAsset,
                                                                                          const PrefabInstanceId instance,
                                                                                          const PrefabLimitProfile &limits,
                                                                                          const CancellationToken &cancellation) const {
        PrefabMetricOperation observation{metrics_, PrefabMetricPhase::Resolve};
        auto result = [&]() -> Result<PrefabSourceOverrideResolution> {
            try {
                return ResolveOverridesImpl(rootAsset, instance, limits, cancellation);
            } catch (const std::bad_alloc &) {
                return Result<PrefabSourceOverrideResolution>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
            }
        }();
        if (result.HasError())
            observation.Fail(result.ErrorValue());
        else if (result.Value().effective) {
            observation.Observe(PrefabMetricItem::ExpandedObjects, result.Value().effective->Objects().size());
            observation.Observe(PrefabMetricItem::GraphNodes, result.Value().effective->Revision().dependencies.size());
            observation.Observe(PrefabMetricItem::GraphEdges, result.Value().effective->Revision().edges.size());
            observation.Complete();
        } else
            observation.Complete();
        return result;
    }

    /** @brief Performs operation-owned materialization only after complete
     * graph/expansion preflight. */
    Result<PrefabSourceOverrideResolution> PrefabSourceResolverSnapshot::ResolveOverridesImpl(const Assets::AssetId rootAsset,
                                                                                              const PrefabInstanceId instance,
                                                                                              const PrefabLimitProfile &limits,
                                                                                              const CancellationToken &cancellation) const {
        if (cancellation.IsCancellationRequested())
            return Result<PrefabSourceOverrideResolution>::Failure(MakeError(PrefabErrors::Cancelled));
        if (!instance.IsValid())
            return Result<PrefabSourceOverrideResolution>::Failure(MakeError(PrefabErrors::IdentityInvalid));
        const PrefabDependencySource *root = FindSource(sources_, rootAsset);
        if (root == nullptr)
            return Result<PrefabSourceOverrideResolution>::Failure(MakeError(PrefabErrors::DependencyUnavailable));

        PrefabExpansionBudget budget{limits};
        std::vector<LocalObjectId> scope;
        std::vector<Assets::AssetId> active;
        std::vector<Assets::AssetId> failureChain;
        std::vector<ResolvedPrefabObject> objects;
        std::vector<PrefabLayerOverrideEvidence> layers;
        objects.reserve(std::min<std::size_t>(sources_.size(), limits.Policy().maximumObjectCount));
        ExpansionState state{sources_, instance, scope,        active, objects,        failureChain,
                             budget,   limits,   cancellation, {},     schemas_.get(), layers};
        if (auto expanded = ExpandAdmittedSource(state, *root); expanded.HasError())
            return Result<PrefabSourceOverrideResolution>::Failure(expanded.ErrorValue());

        auto revision = CaptureRevision(rootAsset, budget);
        if (revision.HasError())
            return Result<PrefabSourceOverrideResolution>::Failure(revision.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<PrefabSourceOverrideResolution>::Failure(MakeError(PrefabErrors::Cancelled));
        auto captured = std::move(revision).Value();
        if (state.unresolved || !captured.revisionSkews.empty())
            return Result<PrefabSourceOverrideResolution>::Success({{}, std::move(layers), std::move(captured)});
        EffectivePrefabCandidate effective{rootAsset, captured, std::move(objects), {}, metrics_};
        return Result<PrefabSourceOverrideResolution>::Success({std::move(effective), std::move(layers), std::move(captured)});
    }

    /** @copydoc PrefabSourceResolverSnapshot::CaptureResolutionRevision */
    Result<PrefabResolutionRevision> PrefabSourceResolverSnapshot::CaptureResolutionRevision(const Assets::AssetId rootAsset,
                                                                                             const PrefabLimitProfile &limits) const {
        PrefabExpansionBudget budget{limits};
        return CaptureRevision(rootAsset, budget);
    }

    /** @copydoc PrefabSourceResolverSnapshot::CaptureRevision */
    Result<PrefabResolutionRevision> PrefabSourceResolverSnapshot::CaptureRevision(const Assets::AssetId rootAsset,
                                                                                   PrefabExpansionBudget &budget) const {
        const auto *root = graph_.FindNode(rootAsset);
        if (root == nullptr || !root->sourceRevision)
            return Result<PrefabResolutionRevision>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
        // Charge the complete graph before the bounded closure walk and its temporary
        // storage.
        if (auto charged = budget.Consume(graph_.Nodes().size() + graph_.Edges().size()); charged.HasError())
            return Result<PrefabResolutionRevision>::Failure(charged.ErrorValue());
        auto closure = graph_.DependencyClosure(std::span{&rootAsset, 1});
        if (closure.HasError())
            return Result<PrefabResolutionRevision>::Failure(closure.ErrorValue());
        auto assets = std::move(closure).Value();
        assets.insert(std::ranges::lower_bound(assets, rootAsset), rootAsset);
        PrefabResolutionRevision revision{graph_.RegistryRevision(), *root->sourceRevision};
        revision.dependencies.reserve(assets.size());
        for (const auto asset : assets) {
            revision.dependencies.push_back(*graph_.FindNode(asset));
            const auto edges = graph_.DirectDependencies(asset);
            revision.edges.insert(revision.edges.end(), edges.begin(), edges.end());
            const auto commitment = std::ranges::lower_bound(commitments_, asset, {}, &PrefabCanonicalSourceCommitment::asset);
            if (commitment != commitments_.end() && commitment->asset == asset)
                revision.canonicalSources.push_back(*commitment);
        }
        CaptureLayerEvidence(revision, graph_, assets, schemas_.get());
        return Result<PrefabResolutionRevision>::Success(std::move(revision));
    }

    /** @copydoc PrefabSourceResolverSnapshot::ValidateRevisionPublication */
    Result<void> PrefabSourceResolverSnapshot::ValidateRevisionPublication(const Assets::AssetId rootAsset,
                                                                           const PrefabResolutionRevision &revision,
                                                                           const std::span<const Assets::AssetId> changedAssets,
                                                                           const PrefabLimitProfile &limits) const {
        PrefabExpansionBudget budget{limits};
        if (auto charged = budget.Consume(revision.dependencies.size() + revision.edges.size() + changedAssets.size()); charged.HasError())
            return charged;
        if (revision.dependencies.empty() || revision.registry.value > RegistryRevision().value)
            return Result<void>::Failure(MakeError(PrefabErrors::ResolutionStale));
        auto current = CaptureRevision(rootAsset, budget);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (!SameContents(revision, current.Value()))
            return Result<void>::Failure(MakeError(PrefabErrors::ResolutionStale));
        for (const auto asset : changedAssets) {
            if (!asset.IsValid())
                return Result<void>::Failure(MakeError(PrefabErrors::IdentityInvalid));
            if (std::ranges::binary_search(revision.dependencies, asset, {}, &PrefabDependencyNode::assetId))
                return Result<void>::Failure(MakeError(PrefabErrors::ResolutionStale));
        }
        return Result<void>::Success();
    }

    /** @copydoc BuildPrefabSourceResolverSnapshot */
    Result<PrefabSourceResolverSnapshot> BuildPrefabSourceResolverSnapshot(const Assets::AssetRegistrySnapshot &registry,
                                                                           std::vector<PrefabDependencySource> sources,
                                                                           const PrefabLimitProfile &limits,
                                                                           const PrefabMetricHandles &metrics,
                                                                           std::shared_ptr<const PrefabValueSchemaSnapshot> schemas,
                                                                           PrefabSourceRevisionPolicy revisionPolicy) {
        try {
            std::ranges::sort(sources, {}, [](const PrefabDependencySource &source) {
                return source.document.Data().assetId;
            });
            auto graph = BuildPrefabDependencyGraph(registry, sources, limits, metrics, revisionPolicy);
            if (graph.HasError())
                return Result<PrefabSourceResolverSnapshot>::Failure(graph.ErrorValue());
            auto commitments = CaptureCanonicalSources(sources);
            if (commitments.HasError())
                return Result<PrefabSourceResolverSnapshot>::Failure(commitments.ErrorValue());
            return Result<PrefabSourceResolverSnapshot>::Success(PrefabSourceResolverSnapshot{std::move(graph).Value(), std::move(sources),
                                                                                              std::move(commitments).Value(), metrics,
                                                                                              std::move(schemas)});
        } catch (const std::bad_alloc &) {
            return Result<PrefabSourceResolverSnapshot>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc ValidatePrefabCandidatePublication */
    Result<void> ValidatePrefabCandidatePublication(const EffectivePrefabCandidate &candidate, const PrefabResolutionRevision &current) {
        if (candidate.Revision() != current)
            return Result<void>::Failure(MakeError(PrefabErrors::ResolutionStale));
        return Result<void>::Success();
    }
}  // namespace Horo::Prefab
