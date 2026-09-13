#include "Horo/Prefab/PrefabSourceResolver.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace Horo::Prefab {
    namespace {
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

        struct ExpansionState final {
            std::span<const PrefabDependencySource> sources;
            PrefabInstanceId instance;
            std::vector<LocalObjectId> &scope;
            std::vector<Assets::AssetId> &active;
            std::vector<ResolvedPrefabObject> &objects;
            PrefabExpansionBudget &budget;
            const PrefabLimitProfile &limits;
        };

        [[nodiscard]] Result<void> ExpandSource(ExpansionState &state, const PrefabDependencySource &source, std::size_t nestedDepth,
                                                std::size_t variantDepth);

        [[nodiscard]] Result<void> MaterializeVariantParent(ExpansionState &state, const PrefabDependencySource &source,
                                                            const std::size_t nestedDepth, const std::size_t variantDepth) {
            if (variantDepth >= state.limits.Policy().maximumVariantInheritanceDepth)
                return Result<void>::Failure(MakeError(PrefabErrors::HierarchyDepthExceeded));
            const PrefabDocumentData &document = source.document.Data();
            const PrefabDependencySource *parent = FindSource(state.sources, document.composition->variantParent->Asset());
            if (parent == nullptr)
                return Result<void>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
            return ExpandSource(state, *parent, nestedDepth, variantDepth + 1);
        }

        [[nodiscard]] Result<void> MaterializeLocalObjects(ExpansionState &state, const PrefabDocumentData &document) {
            for (const PrefabObjectNode &object : document.objects) {
                if (state.objects.size() >= state.limits.Policy().maximumObjectCount)
                    return Result<void>::Failure(MakeError(PrefabErrors::ObjectCountExceeded));
                if (const auto charged = state.budget.Consume(1); charged.HasError())
                    return charged;
                auto address = PrefabObjectAddress::Create(state.scope, object.localId);
                if (address.HasError())
                    return Result<void>::Failure(address.ErrorValue());
                state.objects.emplace_back(document.assetId, ExpandedPrefabObjectKey{state.instance, std::move(address).Value()}, object);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MaterializeNestedPlacements(ExpansionState &state, const PrefabDocumentData &document,
                                                               const std::size_t nestedDepth, const std::size_t variantDepth) {
            if (!document.composition)
                return Result<void>::Success();
            for (const NestedPrefabPlacement &placement : document.composition->nestedPlacements) {
                if (nestedDepth >= state.limits.Policy().maximumNestedPrefabDepth)
                    return Result<void>::Failure(MakeError(PrefabErrors::HierarchyDepthExceeded));
                const PrefabDependencySource *nested = FindSource(state.sources, placement.sourcePrefab.Asset());
                if (nested == nullptr)
                    return Result<void>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
                state.scope.push_back(placement.placementLocalId);
                const PopBackGuard scopeGuard{state.scope};
                if (auto expanded = ExpandSource(state, *nested, nestedDepth + 1, variantDepth); expanded.HasError())
                    return expanded;
            }
            return Result<void>::Success();
        }

        /** @brief Recursively materializes one source using only immutable resolver-owned documents. */
        [[nodiscard]] Result<void> ExpandSource(ExpansionState &state, const PrefabDependencySource &source, const std::size_t nestedDepth,
                                                const std::size_t variantDepth) {
            if (std::ranges::find(state.active, source.document.Data().assetId) != state.active.end())
                return Result<void>::Failure(MakeError(PrefabErrors::DependencyGraphInvalid));
            state.active.push_back(source.document.Data().assetId);
            const PopBackGuard activeGuard{state.active};

            const PrefabDocumentData &document = source.document.Data();
            if (document.composition && document.composition->variantParent)
                return MaterializeVariantParent(state, source, nestedDepth, variantDepth);
            if (auto local = MaterializeLocalObjects(state, document); local.HasError())
                return local;
            return MaterializeNestedPlacements(state, document, nestedDepth, variantDepth);
        }
    }  // namespace

    /** @copydoc EffectivePrefabCandidate::EffectivePrefabCandidate */
    EffectivePrefabCandidate::EffectivePrefabCandidate(Assets::AssetId rootAsset, PrefabResolutionRevision revision,
                                                       std::vector<ResolvedPrefabObject> objects) noexcept
        : rootAsset_(rootAsset), revision_(std::move(revision)), objects_(std::move(objects)) {}

    /** @copydoc EffectivePrefabCandidate::RootAsset */
    Assets::AssetId EffectivePrefabCandidate::RootAsset() const noexcept {
        return rootAsset_;
    }

    /** @copydoc EffectivePrefabCandidate::Revision */
    const PrefabResolutionRevision &EffectivePrefabCandidate::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc EffectivePrefabCandidate::Objects */
    std::span<const ResolvedPrefabObject> EffectivePrefabCandidate::Objects() const noexcept {
        return objects_;
    }

    /** @copydoc PrefabSourceResolverSnapshot::PrefabSourceResolverSnapshot */
    PrefabSourceResolverSnapshot::PrefabSourceResolverSnapshot(PrefabDependencyGraphSnapshot graph,
                                                               std::vector<PrefabDependencySource> sources) noexcept
        : graph_(std::move(graph)), sources_(std::move(sources)) {}

    /** @copydoc PrefabSourceResolverSnapshot::RegistryRevision */
    Assets::AssetRegistryRevision PrefabSourceResolverSnapshot::RegistryRevision() const noexcept {
        return graph_.RegistryRevision();
    }

    /** @copydoc PrefabSourceResolverSnapshot::Sources */
    std::span<const PrefabDependencySource> PrefabSourceResolverSnapshot::Sources() const noexcept {
        return sources_;
    }

    /** @copydoc PrefabSourceResolverSnapshot::Resolve */
    Result<EffectivePrefabCandidate> PrefabSourceResolverSnapshot::Resolve(const Assets::AssetId rootAsset, const PrefabInstanceId instance,
                                                                           const PrefabLimitProfile &limits) const {
        if (!instance.IsValid())
            return Result<EffectivePrefabCandidate>::Failure(MakeError(PrefabErrors::IdentityInvalid));
        const PrefabDependencySource *root = FindSource(sources_, rootAsset);
        if (root == nullptr)
            return Result<EffectivePrefabCandidate>::Failure(MakeError(PrefabErrors::DependencyUnavailable));

        PrefabExpansionBudget budget{limits};
        std::vector<LocalObjectId> scope;
        std::vector<Assets::AssetId> active;
        std::vector<ResolvedPrefabObject> objects;
        objects.reserve(std::min<std::size_t>(sources_.size(), limits.Policy().maximumObjectCount));
        ExpansionState state{sources_, instance, scope, active, objects, budget, limits};
        if (auto expanded = ExpandSource(state, *root, 1, 1); expanded.HasError())
            return Result<EffectivePrefabCandidate>::Failure(expanded.ErrorValue());

        return Result<EffectivePrefabCandidate>::Success(
            EffectivePrefabCandidate{rootAsset, PrefabResolutionRevision{graph_.RegistryRevision(), root->sourceRevision},
                                     std::move(objects)});
    }

    /** @copydoc BuildPrefabSourceResolverSnapshot */
    Result<PrefabSourceResolverSnapshot> BuildPrefabSourceResolverSnapshot(const Assets::AssetRegistrySnapshot &registry,
                                                                           std::vector<PrefabDependencySource> sources,
                                                                           const PrefabLimitProfile &limits) {
        std::ranges::sort(sources, {}, [](const PrefabDependencySource &source) {
            return source.document.Data().assetId;
        });
        auto graph = BuildPrefabDependencyGraph(registry, sources, limits);
        if (graph.HasError())
            return Result<PrefabSourceResolverSnapshot>::Failure(graph.ErrorValue());
        return Result<PrefabSourceResolverSnapshot>::Success(PrefabSourceResolverSnapshot{std::move(graph).Value(), std::move(sources)});
    }

    /** @copydoc ValidatePrefabCandidatePublication */
    Result<void> ValidatePrefabCandidatePublication(const EffectivePrefabCandidate &candidate, const PrefabResolutionRevision &current) {
        if (candidate.Revision() != current)
            return Result<void>::Failure(MakeError(PrefabErrors::ResolutionStale));
        return Result<void>::Success();
    }
}  // namespace Horo::Prefab
