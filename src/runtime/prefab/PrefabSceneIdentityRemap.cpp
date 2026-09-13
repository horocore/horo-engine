#include "Horo/Prefab/PrefabSceneIdentityRemap.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Horo::Prefab {
    namespace {
        constexpr std::uint64_t FnvOffsetBasis = 14695981039346656037ULL;
        constexpr std::uint64_t FnvPrime = 1099511628211ULL;
        constexpr std::array<std::byte, 8> IdentityDomain{std::byte{'H'}, std::byte{'P'}, std::byte{'F'}, std::byte{'B'},
                                                          std::byte{'S'}, std::byte{'I'}, std::byte{'D'}, std::byte{1}};

        /** @brief Hashes canonical bytes with the versioned FNV-1a identity contract. */
        void HashBytes(std::uint64_t &hash, const std::span<const std::byte> bytes) noexcept {
            for (const std::byte value : bytes) {
                hash ^= std::to_integer<std::uint8_t>(value);
                hash *= FnvPrime;
            }
        }

        /** @brief Appends one unsigned integer in canonical little-endian byte order. */
        template <typename ValueT> void HashInteger(std::uint64_t &hash, const ValueT value) noexcept {
            static_assert(std::is_unsigned_v<ValueT>);
            for (std::size_t index = 0; index < sizeof(ValueT); ++index) {
                hash ^= static_cast<std::uint8_t>(value >> (index * 8U));
                hash *= FnvPrime;
            }
        }

        /** @brief Maps one stable expanded object key with no process-local hash dependency. */
        [[nodiscard]] PrefabSceneObjectId HashIdentity(const ExpandedPrefabObjectKey &key) noexcept {
            std::uint64_t hash = FnvOffsetBasis;
            HashBytes(hash, IdentityDomain);
            HashInteger(hash, PrefabSceneIdentityHashVersion);
            HashInteger(hash, key.instance.Value());
            HashInteger(hash, static_cast<std::uint32_t>(key.object.NestedInstanceScope().size()));
            for (const LocalObjectId segment : key.object.NestedInstanceScope())
                HashInteger(hash, segment.value);
            HashInteger(hash, key.object.SourceObject().value);
            return {hash == 0 ? 1 : hash};
        }

        struct ExpandedKeyHash final {
            /** @brief Hashes an expanded key for operation-local lookup; iteration order is never observed. */
            [[nodiscard]] std::size_t operator()(const ExpandedPrefabObjectKey &key) const noexcept {
                return static_cast<std::size_t>(HashIdentity(key).value);
            }
        };

        using IdentityLookup = std::unordered_map<ExpandedPrefabObjectKey, PrefabSceneObjectId, ExpandedKeyHash>;

        /** @brief Finds a mapped object in source-key order. */
        [[nodiscard]] const PrefabSceneIdentityMapping *FindMapping(const std::span<const PrefabSceneIdentityMapping> mappings,
                                                                    const ExpandedPrefabObjectKey &source) noexcept {
            const auto found = std::ranges::lower_bound(mappings, source, {}, &PrefabSceneIdentityMapping::source);
            return found != mappings.end() && found->source == source ? std::addressof(*found) : nullptr;
        }

        /** @brief Validates one request's mutually exclusive typed target fields. */
        [[nodiscard]] bool IsCanonical(const PrefabReferenceRewriteRequest &request) noexcept {
            const bool object = request.objectTarget.has_value();
            const bool component = request.componentTarget.has_value();
            const bool behavior = request.behaviorTarget.has_value();
            const bool asset = request.assetTarget.has_value();
            switch (request.kind) {
                case PrefabReferenceKind::Entity:
                    return object && !component && !behavior && !asset;
                case PrefabReferenceKind::Component:
                    return object && component && request.componentTarget->IsValid() && !behavior && !asset;
                case PrefabReferenceKind::Behavior:
                    return object && !component && behavior && request.behaviorTarget->IsValid() && !asset;
                case PrefabReferenceKind::Asset:
                    return !object && !component && !behavior && asset && request.assetTarget->IsValid();
            }
            return false;
        }

        /** @brief Rewrites all typed requests without publishing partial output on failure. */
        [[nodiscard]] Result<std::vector<RewrittenPrefabReference>> RewriteReferences(
            const std::span<const PrefabReferenceRewriteRequest> references, const IdentityLookup &lookup) {
            std::vector<RewrittenPrefabReference> rewritten;
            rewritten.reserve(references.size());
            for (const PrefabReferenceRewriteRequest &request : references) {
                if (!IsCanonical(request))
                    return Result<std::vector<RewrittenPrefabReference>>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                const auto owner = lookup.find(request.owner);
                if (owner == lookup.end())
                    return Result<std::vector<RewrittenPrefabReference>>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));

                std::optional<PrefabSceneObjectId> target;
                if (request.objectTarget) {
                    const auto mappedTarget = lookup.find(*request.objectTarget);
                    if (mappedTarget == lookup.end())
                        return Result<std::vector<RewrittenPrefabReference>>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                    target = mappedTarget->second;
                }
                rewritten.push_back(
                    {owner->second, request.kind, target, request.componentTarget, request.behaviorTarget, request.assetTarget});
            }
            return Result<std::vector<RewrittenPrefabReference>>::Success(std::move(rewritten));
        }
    }  // namespace

    /** @copydoc PrefabSceneIdentityMap::PrefabSceneIdentityMap */
    PrefabSceneIdentityMap::PrefabSceneIdentityMap(PrefabResolutionRevision revision, std::vector<PrefabSceneIdentityMapping> mappings,
                                                   std::vector<RewrittenPrefabReference> references) noexcept
        : revision_(std::move(revision)), mappings_(std::move(mappings)), references_(std::move(references)) {}

    /** @copydoc PrefabSceneIdentityMap::Revision */
    const PrefabResolutionRevision &PrefabSceneIdentityMap::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc PrefabSceneIdentityMap::Mappings */
    std::span<const PrefabSceneIdentityMapping> PrefabSceneIdentityMap::Mappings() const noexcept {
        return mappings_;
    }

    /** @copydoc PrefabSceneIdentityMap::References */
    std::span<const RewrittenPrefabReference> PrefabSceneIdentityMap::References() const noexcept {
        return references_;
    }

    /** @copydoc PrefabSceneIdentityMap::Find */
    std::optional<PrefabSceneObjectId> PrefabSceneIdentityMap::Find(const ExpandedPrefabObjectKey &source) const noexcept {
        const PrefabSceneIdentityMapping *mapping = FindMapping(mappings_, source);
        return mapping == nullptr ? std::nullopt : std::optional{mapping->scene};
    }

    /** @copydoc RemapPrefabCandidateToScene */
    Result<PrefabSceneIdentityMap> RemapPrefabCandidateToScene(const EffectivePrefabCandidate &candidate,
                                                               const std::span<const PrefabSceneObjectId> occupied,
                                                               const std::span<const PrefabReferenceRewriteRequest> references,
                                                               const PrefabLimitProfile &limits) {
        if (candidate.Objects().size() > limits.Policy().maximumObjectCount)
            return Result<PrefabSceneIdentityMap>::Failure(MakeError(PrefabErrors::ObjectCountExceeded));
        if (references.size() > limits.Policy().maximumReferencedAssets)
            return Result<PrefabSceneIdentityMap>::Failure(MakeError(PrefabErrors::ReferenceCountExceeded));

        std::unordered_set<std::uint64_t> identities;
        identities.reserve(occupied.size());
        for (const PrefabSceneObjectId identity : occupied) {
            if (!identity.IsValid() || !identities.insert(identity.value).second)
                return Result<PrefabSceneIdentityMap>::Failure(MakeError(PrefabErrors::IdentityCollision));
        }

        std::vector<PrefabSceneIdentityMapping> mappings;
        mappings.reserve(candidate.Objects().size());
        IdentityLookup lookup;
        lookup.reserve(candidate.Objects().size());
        for (const ResolvedPrefabObject &object : candidate.Objects()) {
            const PrefabSceneObjectId scene = HashIdentity(object.key);
            if (!identities.insert(scene.value).second || !lookup.emplace(object.key, scene).second)
                return Result<PrefabSceneIdentityMap>::Failure(MakeError(PrefabErrors::IdentityCollision));
            mappings.push_back({object.key, object.sourcePrefab, scene});
        }
        std::ranges::sort(mappings, {}, &PrefabSceneIdentityMapping::source);

        auto rewritten = RewriteReferences(references, lookup);
        if (rewritten.HasError())
            return Result<PrefabSceneIdentityMap>::Failure(rewritten.ErrorValue());

        return Result<PrefabSceneIdentityMap>::Success(
            PrefabSceneIdentityMap{candidate.Revision(), std::move(mappings), std::move(rewritten).Value()});
    }
}  // namespace Horo::Prefab
