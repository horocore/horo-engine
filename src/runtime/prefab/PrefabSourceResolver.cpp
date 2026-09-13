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

        [[nodiscard]] Result<void> ExpandSource(std::span<const PrefabDependencySource> sources, const PrefabDependencySource &source,
                                                PrefabInstanceId instance, std::vector<LocalObjectId> &scope,
                                                std::vector<Assets::AssetId> &active, std::vector<ResolvedPrefabObject> &objects,
                                                PrefabExpansionBudget &budget, const PrefabLimitProfile &limits, std::size_t nestedDepth,
                                                std::size_t variantDepth);

        [[nodiscard]] Result<void> MaterializeVariantParent(const std::span<const PrefabDependencySource> sources,
                                                            const PrefabDependencySource &source, const PrefabInstanceId instance,
                                                            std::vector<LocalObjectId> &scope, std::vector<Assets::AssetId> &active,
                                                            std::vector<ResolvedPrefabObject> &objects, PrefabExpansionBudget &budget,
                                                            const PrefabLimitProfile &limits, const std::size_t nestedDepth,
                                                            const std::size_t variantDepth) {
            if (variantDepth >= limits.Policy().maximumVariantInheritanceDepth)
                return Result<void>::Failure(MakeError(PrefabErrors::HierarchyDepthExceeded));
            const PrefabDocumentData &document = source.document.Data();
            const PrefabDependencySource *parent = FindSource(sources, document.composition->variantParent->Asset());
            if (parent == nullptr)
                return Result<void>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
            return ExpandSource(sources, *parent, instance, scope, active, objects, budget, limits, nestedDepth, variantDepth + 1);
        }

        [[nodiscard]] Result<void> MaterializeLocalObjects(const PrefabDocumentData &document, const PrefabInstanceId instance,
                                                           const std::span<const LocalObjectId> scope,
                                                           std::vector<ResolvedPrefabObject> &objects, PrefabExpansionBudget &budget,
                                                           const PrefabLimitProfile &limits) {
            for (const PrefabObjectNode &object : document.objects) {
                if (objects.size() >= limits.Policy().maximumObjectCount)
                    return Result<void>::Failure(MakeError(PrefabErrors::ObjectCountExceeded));
                if (const auto charged = budget.Consume(1); charged.HasError())
                    return charged;
                auto address = PrefabObjectAddress::Create(scope, object.localId);
                if (address.HasError())
                    return Result<void>::Failure(address.ErrorValue());
                objects.push_back({document.assetId, ExpandedPrefabObjectKey{instance, std::move(address).Value()}, object});
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MaterializeNestedPlacements(const std::span<const PrefabDependencySource> sources,
                                                               const PrefabDocumentData &document, const PrefabInstanceId instance,
                                                               std::vector<LocalObjectId> &scope, std::vector<Assets::AssetId> &active,
                                                               std::vector<ResolvedPrefabObject> &objects, PrefabExpansionBudget &budget,
                                                               const PrefabLimitProfile &limits, const std::size_t nestedDepth,
                                                               const std::size_t variantDepth) {
            if (!document.composition)
                return Result<void>::Success();
            for (const NestedPrefabPlacement &placement : document.composition->nestedPlacements) {
                if (nestedDepth >= limits.Policy().maximumNestedPrefabDepth)
                    return Result<void>::Failure(MakeError(PrefabErrors::HierarchyDepthExceeded));
                const PrefabDependencySource *nested = FindSource(sources, placement.sourcePrefab.Asset());
                if (nested == nullptr)
                    return Result<void>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
                scope.push_back(placement.placementLocalId);
                const PopBackGuard scopeGuard{scope};
                if (auto expanded =
                        ExpandSource(sources, *nested, instance, scope, active, objects, budget, limits, nestedDepth + 1, variantDepth);
                    expanded.HasError())
                    return expanded;
            }
            return Result<void>::Success();
        }

        /** @brief Recursively materializes one source using only immutable resolver-owned documents. */
        [[nodiscard]] Result<void> ExpandSource(const std::span<const PrefabDependencySource> sources, const PrefabDependencySource &source,
                                                const PrefabInstanceId instance, std::vector<LocalObjectId> &scope,
                                                std::vector<Assets::AssetId> &active, std::vector<ResolvedPrefabObject> &objects,
                                                PrefabExpansionBudget &budget, const PrefabLimitProfile &limits,
                                                const std::size_t nestedDepth, const std::size_t variantDepth) {
            if (std::ranges::find(active, source.document.Data().assetId) != active.end())
                return Result<void>::Failure(MakeError(PrefabErrors::DependencyGraphInvalid));
            active.push_back(source.document.Data().assetId);
            const PopBackGuard activeGuard{active};

            const PrefabDocumentData &document = source.document.Data();
            if (document.composition && document.composition->variantParent)
                return MaterializeVariantParent(sources, source, instance, scope, active, objects, budget, limits, nestedDepth,
                                                variantDepth);
            if (auto local = MaterializeLocalObjects(document, instance, scope, objects, budget, limits); local.HasError())
                return local;
            return MaterializeNestedPlacements(sources, document, instance, scope, active, objects, budget, limits, nestedDepth,
                                               variantDepth);
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
        if (auto expanded = ExpandSource(sources_, *root, instance, scope, active, objects, budget, limits, 1, 1); expanded.HasError())
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
