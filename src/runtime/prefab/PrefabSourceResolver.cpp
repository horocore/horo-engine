#include "Horo/Prefab/PrefabSourceResolver.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <string>
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
            std::vector<Assets::AssetId> &failureChain;
            PrefabExpansionBudget &budget;
            const PrefabLimitProfile &limits;
        };

        /** @brief Records the complete source path that made a required expansion fail. */
        [[nodiscard]] Result<void> Fail(ExpansionState &state, Error error, const std::optional<Assets::AssetId> next = std::nullopt) {
            state.failureChain = state.active;
            if (next)
                state.failureChain.push_back(*next);
            return Result<void>::Failure(std::move(error));
        }

        /** @brief Adds immutable operation context without changing the underlying typed failure. */
        void AddFailureContext(Error &error, const PrefabInstanceId instance, const std::span<const Assets::AssetId> sourceChain) {
            std::string context = std::format("Prefab instance {} source chain", instance.Value());
            for (const Assets::AssetId source : sourceChain)
                context += " -> " + source.ToString();
            error.message = context + ": " + error.message;
        }

        [[nodiscard]] Result<void> ExpandSource(ExpansionState &state, const PrefabDependencySource &source, std::size_t nestedDepth,
                                                std::size_t variantDepth,
                                                const std::optional<ExpandedPrefabObjectKey> &mountParent = std::nullopt,
                                                const Math::Transform &mountTransform = {});

        [[nodiscard]] Result<void> MaterializeVariantParent(ExpansionState &state, const PrefabDependencySource &source,
                                                            const std::size_t nestedDepth, const std::size_t variantDepth,
                                                            const std::optional<ExpandedPrefabObjectKey> &mountParent,
                                                            const Math::Transform &mountTransform) {
            if (variantDepth >= state.limits.Policy().maximumVariantInheritanceDepth)
                return Fail(state, MakeError(PrefabErrors::HierarchyDepthExceeded));
            const PrefabDocumentData &document = source.document.Data();
            const Assets::AssetId parentAsset = document.composition->variantParent->Asset();
            const PrefabDependencySource *parent = FindSource(state.sources, parentAsset);
            if (parent == nullptr)
                return Fail(state, MakeError(PrefabErrors::DependencyUnavailable), parentAsset);
            return ExpandSource(state, *parent, nestedDepth, variantDepth + 1, mountParent, mountTransform);
        }

        [[nodiscard]] Result<void> MaterializeLocalObjects(ExpansionState &state, const PrefabDocumentData &document,
                                                           const std::optional<ExpandedPrefabObjectKey> &mountParent,
                                                           const Math::Transform &mountTransform) {
            for (const PrefabObjectNode &object : document.objects) {
                if (state.objects.size() >= state.limits.Policy().maximumObjectCount)
                    return Fail(state, MakeError(PrefabErrors::ObjectCountExceeded));
                if (const auto charged = state.budget.Consume(1); charged.HasError())
                    return Fail(state, charged.ErrorValue());
                auto address = PrefabObjectAddress::Create(state.scope, object.localId);
                if (address.HasError())
                    return Fail(state, address.ErrorValue());
                const ExpandedPrefabObjectKey key{state.instance, std::move(address).Value()};
                std::optional<ExpandedPrefabObjectKey> parent = mountParent;
                Math::Transform localTransform = object.localTransform;
                if (object.parentLocalId) {
                    auto parentAddress = PrefabObjectAddress::Create(state.scope, *object.parentLocalId);
                    if (parentAddress.HasError())
                        return Fail(state, parentAddress.ErrorValue());
                    parent = ExpandedPrefabObjectKey{state.instance, std::move(parentAddress).Value()};
                }
                if (mountParent && !object.parentLocalId) {
                    const auto composed =
                        Math::TryDecomposeAffineTRS(Math::Multiply(mountTransform.ToMatrix(), object.localTransform.ToMatrix()));
                    if (composed.HasError())
                        return Fail(state, composed.ErrorValue());
                    localTransform = composed.Value();
                }
                state.objects.emplace_back(document.assetId, key, parent, object, localTransform);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MaterializeNestedPlacements(ExpansionState &state, const PrefabDocumentData &document,
                                                               const std::size_t nestedDepth, const std::size_t variantDepth,
                                                               const std::optional<ExpandedPrefabObjectKey> &sourceRoot) {
            if (!document.composition)
                return Result<void>::Success();
            for (const NestedPrefabPlacement &placement : document.composition->nestedPlacements) {
                if (nestedDepth >= state.limits.Policy().maximumNestedPrefabDepth)
                    return Fail(state, MakeError(PrefabErrors::HierarchyDepthExceeded));
                const Assets::AssetId nestedAsset = placement.sourcePrefab.Asset();
                const PrefabDependencySource *nested = FindSource(state.sources, nestedAsset);
                if (nested == nullptr)
                    return Fail(state, MakeError(PrefabErrors::DependencyUnavailable), nestedAsset);
                std::optional<ExpandedPrefabObjectKey> mountParent = sourceRoot;
                if (placement.parentLocalId) {
                    auto parentAddress = PrefabObjectAddress::Create(state.scope, *placement.parentLocalId);
                    if (parentAddress.HasError())
                        return Fail(state, parentAddress.ErrorValue());
                    mountParent = ExpandedPrefabObjectKey{state.instance, std::move(parentAddress).Value()};
                }
                state.scope.push_back(placement.placementLocalId);
                const PopBackGuard scopeGuard{state.scope};
                if (auto expanded = ExpandSource(state, *nested, nestedDepth + 1, variantDepth, mountParent, placement.localRootTransform);
                    expanded.HasError())
                    return expanded;
            }
            return Result<void>::Success();
        }

        /** @brief Recursively materializes one source using only immutable resolver-owned documents. */
        [[nodiscard]] Result<void> ExpandSource(ExpansionState &state, const PrefabDependencySource &source, const std::size_t nestedDepth,
                                                const std::size_t variantDepth, const std::optional<ExpandedPrefabObjectKey> &mountParent,
                                                const Math::Transform &mountTransform) {
            if (const Assets::AssetId sourceAsset = source.document.Data().assetId;
                std::ranges::find(state.active, sourceAsset) != state.active.end())
                return Fail(state, MakeError(PrefabErrors::DependencyGraphInvalid), sourceAsset);
            state.active.push_back(source.document.Data().assetId);
            const PopBackGuard activeGuard{state.active};

            const PrefabDocumentData &document = source.document.Data();
            if (document.composition && document.composition->variantParent)
                return MaterializeVariantParent(state, source, nestedDepth, variantDepth, mountParent, mountTransform);
            if (auto local = MaterializeLocalObjects(state, document, mountParent, mountTransform); local.HasError())
                return local;
            const auto rootAddress = PrefabObjectAddress::Create(state.scope, LocalObjectId{});
            if (rootAddress.HasError())
                return Fail(state, rootAddress.ErrorValue());
            return MaterializeNestedPlacements(state, document, nestedDepth, variantDepth,
                                               ExpandedPrefabObjectKey{state.instance, rootAddress.Value()});
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
        std::vector<Assets::AssetId> failureChain;
        std::vector<ResolvedPrefabObject> objects;
        objects.reserve(std::min<std::size_t>(sources_.size(), limits.Policy().maximumObjectCount));
        ExpansionState state{sources_, instance, scope, active, objects, failureChain, budget, limits};
        if (auto expanded = ExpandSource(state, *root, 1, 1); expanded.HasError()) {
            Error error = expanded.ErrorValue();
            if (failureChain.empty())
                failureChain.push_back(rootAsset);
            AddFailureContext(error, instance, failureChain);
            return Result<EffectivePrefabCandidate>::Failure(std::move(error));
        }

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
