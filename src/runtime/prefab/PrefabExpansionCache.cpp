#include "Horo/Prefab/PrefabExpansionCache.h"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>

namespace Horo::Prefab {
    namespace {
        /** @brief Charges logical retained allocations without overflowing or crossing the host budget. */
        class StorageBudget final {
        public:
            explicit StorageBudget(const std::size_t maximum) : maximum_(maximum) {}

            bool Add(const std::size_t count, const std::size_t width = 1) {
                if (width == 0 || count > (maximum_ - used_) / width)
                    return false;
                used_ += count * width;
                return true;
            }

            std::size_t Used() const noexcept {
                return used_;
            }

        private:
            std::size_t maximum_;
            std::size_t used_{};
        };

        /** @brief Includes nested owned behavior field strings and vector storage in the retained allocation estimate. */
        bool ChargeBehavior(StorageBudget &budget, const Gameplay::BehaviorComponent &behavior) {
            if (!budget.Add(behavior.typeId.Value().capacity()) || !budget.Add(behavior.fields.capacity(), sizeof(Gameplay::BehaviorField)))
                return false;
            for (const auto &field : behavior.fields) {
                if (!budget.Add(field.name.capacity()))
                    return false;
                if (const auto *text = std::get_if<std::string>(&field.value); text && !budget.Add(text->capacity()))
                    return false;
            }
            return true;
        }

        /** @brief Charges one complete object's owned payloads separately from dependency metadata. */
        bool ChargeObject(StorageBudget &budget, const PrefabObjectNode &object) {
            if (!budget.Add(object.name.capacity()) || !budget.Add(object.components.capacity(), sizeof(RawComponentPayload)) ||
                !budget.Add(object.behaviors.capacity(), sizeof(Gameplay::BehaviorComponent)))
                return false;
            for (const auto &component : object.components) {
                if (!budget.Add(component.component.typeId.Value().capacity()) || !budget.Add(component.component.payload.capacity()))
                    return false;
            }
            for (const auto &behavior : object.behaviors) {
                if (!ChargeBehavior(budget, behavior))
                    return false;
            }
            return true;
        }

        /** @brief Measures complete immutable candidate ownership, including all variable payload storage. */
        bool ChargeCandidate(StorageBudget &budget, const EffectivePrefabCandidate &candidate) {
            const auto &revision = candidate.Revision();
            if (!budget.Add(sizeof(EffectivePrefabCandidate)) ||
                !budget.Add(revision.dependencies.capacity(), sizeof(PrefabDependencyNode)) ||
                !budget.Add(revision.edges.capacity(), sizeof(PrefabDependencyEdge)) ||
                !budget.Add(candidate.Objects().size(), sizeof(ResolvedPrefabObject)))
                return false;
            for (const auto &node : revision.dependencies) {
                if (!budget.Add(node.assetType.Value().capacity()))
                    return false;
            }
            for (const auto &resolved : candidate.Objects()) {
                if (!ChargeObject(budget, resolved.object))
                    return false;
            }
            return true;
        }

        /** @brief Rejects invalid host ceilings consistently before key capture or result retention. */
        bool ValidPolicy(const PrefabExpansionCacheLimits &limits) noexcept {
            return limits.maximumEntries > 0 && limits.maximumEntries <= 256 && limits.maximumRetainedBytes > 0 &&
                   limits.maximumKeySourceBytes > 0 && limits.maximumKeySourceBytes <= PrefabHardLimits::SourceDocumentBytes;
        }

        /** @brief Associates bounded canonical commitments with source-producing dependency identities. */
        Result<std::vector<Sha256Digest>> SourceCommitments(const PrefabSourceResolverSnapshot &resolver,
                                                            const PrefabResolutionRevision &revision, const std::size_t maximumBytes,
                                                            const CancellationToken &cancellation) {
            using Digests = std::vector<Sha256Digest>;
            StorageBudget budget{maximumBytes};
            Digests digests;
            digests.reserve(revision.dependencies.size());
            for (const auto &dependency : revision.dependencies) {
                if (!dependency.sourceRevision)
                    continue;
                const auto commitments = resolver.CanonicalSourceCommitments();
                const auto source = std::ranges::lower_bound(commitments, dependency.assetId, {}, &PrefabCanonicalSourceCommitment::asset);
                if (source == commitments.end() || source->asset != dependency.assetId)
                    return Result<Digests>::Failure(MakeError(PrefabErrors::ResolutionStale));
                if (cancellation.IsCancellationRequested())
                    return Result<Digests>::Failure(MakeError(PrefabErrors::Cancelled));
                if (!budget.Add(source->encodedBytes))
                    return Result<Digests>::Failure(MakeError(PrefabErrors::ExpansionCacheCapacityExceeded));
                digests.push_back(source->digest);
            }
            return Result<Digests>::Success(std::move(digests));
        }
    }  // namespace

    /** @copydoc PrefabExpansionCache::PrefabExpansionCache */
    PrefabExpansionCache::PrefabExpansionCache(const PrefabExpansionCacheLimits limits)
        : limits_(limits), owner_(std::this_thread::get_id()) {}

    bool PrefabExpansionCache::IsOwner() const noexcept {
        return owner_ == std::this_thread::get_id();
    }

    /** @copydoc PrefabExpansionCache::CaptureKey */
    Result<PrefabExpansionCacheKey> PrefabExpansionCache::CaptureKey(const PrefabSourceResolverSnapshot &resolver,
                                                                     const Assets::AssetId root, const PrefabInstanceId instance,
                                                                     const PrefabLimitProfile &limits,
                                                                     const CancellationToken &cancellation) const {
        try {
            return CaptureKeyImpl(resolver, root, instance, limits, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<PrefabExpansionCacheKey>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    Result<PrefabExpansionCacheKey> PrefabExpansionCache::CaptureKeyImpl(const PrefabSourceResolverSnapshot &resolver,
                                                                         const Assets::AssetId root, const PrefabInstanceId instance,
                                                                         const PrefabLimitProfile &limits,
                                                                         const CancellationToken &cancellation) const {
        if (!IsOwner())
            return Result<PrefabExpansionCacheKey>::Failure(MakeError(PrefabErrors::ExpansionCacheThreadViolation));
        if (!instance.IsValid())
            return Result<PrefabExpansionCacheKey>::Failure(MakeError(PrefabErrors::IdentityInvalid));
        if (!ValidPolicy(limits_))
            return Result<PrefabExpansionCacheKey>::Failure(MakeError(PrefabErrors::ExpansionCacheCapacityExceeded));
        auto revision = resolver.CaptureResolutionRevision(root, limits);
        if (revision.HasError())
            return Result<PrefabExpansionCacheKey>::Failure(revision.ErrorValue());
        auto digests = SourceCommitments(resolver, revision.Value(), limits_.maximumKeySourceBytes, cancellation);
        if (digests.HasError())
            return Result<PrefabExpansionCacheKey>::Failure(digests.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<PrefabExpansionCacheKey>::Failure(MakeError(PrefabErrors::Cancelled));
        return Result<PrefabExpansionCacheKey>::Success(
            PrefabExpansionCacheKey{root, instance, std::move(revision).Value(), limits.Policy(), std::move(digests).Value()});
    }

    /** @copydoc PrefabExpansionCache::Find */
    std::shared_ptr<const EffectivePrefabCandidate> PrefabExpansionCache::Find(const PrefabExpansionCacheKey &key) const {
        if (!IsOwner())
            return {};
        const auto found = std::ranges::find(entries_, key, &Entry::key);
        return found == entries_.end() ? nullptr : found->candidate;
    }

    /** @copydoc PrefabExpansionCache::Store */
    Result<std::shared_ptr<const EffectivePrefabCandidate>> PrefabExpansionCache::Store(PrefabExpansionCacheKey key,
                                                                                        EffectivePrefabCandidate candidate) {
        try {
            return StoreImpl(std::move(key), std::move(candidate));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<const EffectivePrefabCandidate>>::Failure(
                MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    Result<std::shared_ptr<const EffectivePrefabCandidate>> PrefabExpansionCache::StoreImpl(PrefabExpansionCacheKey key,
                                                                                            EffectivePrefabCandidate candidate) {
        using Lease = std::shared_ptr<const EffectivePrefabCandidate>;
        if (!IsOwner())
            return Result<Lease>::Failure(MakeError(PrefabErrors::ExpansionCacheThreadViolation));
        if (!ValidPolicy(limits_))
            return Result<Lease>::Failure(MakeError(PrefabErrors::ExpansionCacheCapacityExceeded));
        if (!Matches(key, candidate))
            return Result<Lease>::Failure(MakeError(PrefabErrors::ResolutionStale));
        if (const auto existing = Find(key))
            return Result<Lease>::Success(existing);
        const auto measured = MeasureEntry(key, candidate);
        if (measured.HasError())
            return Result<Lease>::Failure(measured.ErrorValue());
        // Reserve every throwing allocation before eviction.
        static_assert(std::is_nothrow_move_constructible_v<Entry>);
        static_assert(std::is_nothrow_move_assignable_v<Entry>);
        auto lease = std::make_shared<const EffectivePrefabCandidate>(std::move(candidate));
        entries_.reserve(limits_.maximumEntries);
        while (entries_.size() >= limits_.maximumEntries || retainedBytes_ > limits_.maximumRetainedBytes - measured.Value()) {
            retainedBytes_ -= entries_.front().bytes;
            entries_.erase(entries_.begin());
        }
        entries_.emplace_back(std::move(key), lease, measured.Value());
        retainedBytes_ += measured.Value();
        return Result<Lease>::Success(std::move(lease));
    }

    /** @copydoc PrefabExpansionCache::Matches */
    bool PrefabExpansionCache::Matches(const PrefabExpansionCacheKey &key, const EffectivePrefabCandidate &candidate) const {
        return key.root_ == candidate.RootAsset() && key.revision_ == candidate.Revision() &&
               std::ranges::all_of(candidate.Objects(), [&key](const auto &object) {
            return object.key.instance == key.instance_;
        });
    }

    /** @copydoc PrefabExpansionCache::MeasureEntry */
    Result<std::size_t> PrefabExpansionCache::MeasureEntry(const PrefabExpansionCacheKey &key,
                                                           const EffectivePrefabCandidate &candidate) const {
        StorageBudget budget{limits_.maximumRetainedBytes};
        if (!budget.Add(candidate.objects_.capacity() - candidate.objects_.size(), sizeof(ResolvedPrefabObject)))
            return Result<std::size_t>::Failure(MakeError(PrefabErrors::ExpansionCacheCapacityExceeded));
        if (!budget.Add(sizeof(Entry)) || !budget.Add(key.revision_.dependencies.capacity(), sizeof(PrefabDependencyNode)) ||
            !budget.Add(key.revision_.edges.capacity(), sizeof(PrefabDependencyEdge)) ||
            !budget.Add(key.sources_.capacity(), sizeof(Sha256Digest)) || !ChargeCandidate(budget, candidate))
            return Result<std::size_t>::Failure(MakeError(PrefabErrors::ExpansionCacheCapacityExceeded));
        for (const auto &node : key.revision_.dependencies) {
            if (!budget.Add(node.assetType.Value().capacity()))
                return Result<std::size_t>::Failure(MakeError(PrefabErrors::ExpansionCacheCapacityExceeded));
        }
        return Result<std::size_t>::Success(budget.Used());
    }

    /** @copydoc PrefabExpansionCache::Resolve */
    Result<std::shared_ptr<const EffectivePrefabCandidate>> PrefabExpansionCache::Resolve(const PrefabSourceResolverSnapshot &resolver,
                                                                                          const Assets::AssetId root,
                                                                                          const PrefabInstanceId instance,
                                                                                          const PrefabLimitProfile &limits,
                                                                                          const CancellationToken &cancellation) {
        using Lease = std::shared_ptr<const EffectivePrefabCandidate>;
        try {
            auto key = CaptureKey(resolver, root, instance, limits, cancellation);
            if (key.HasError())
                return Result<Lease>::Failure(key.ErrorValue());
            if (const auto hit = Find(key.Value())) {
                if (cancellation.IsCancellationRequested())
                    return Result<Lease>::Failure(MakeError(PrefabErrors::Cancelled));
                return Result<Lease>::Success(hit);
            }
            auto expanded = resolver.Resolve(root, instance, limits, cancellation);
            if (expanded.HasError())
                return Result<Lease>::Failure(expanded.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Result<Lease>::Failure(MakeError(PrefabErrors::Cancelled));
            return Store(std::move(key).Value(), std::move(expanded).Value());
        } catch (const std::bad_alloc &) {
            return Result<Lease>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PrefabExpansionCache::Clear */
    Result<void> PrefabExpansionCache::Clear() {
        if (!IsOwner())
            return Result<void>::Failure(MakeError(PrefabErrors::ExpansionCacheThreadViolation));
        entries_.clear();
        retainedBytes_ = 0;
        return Result<void>::Success();
    }
}  // namespace Horo::Prefab
