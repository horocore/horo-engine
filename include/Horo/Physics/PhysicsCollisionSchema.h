#pragma once

/**
 * @file PhysicsCollisionSchema.h
 * @brief Project-authored collision policy and immutable normalized Physics schema.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Physics/PhysicsFilterIdentity.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Physics {
    /** @brief Project-authored simulation response for one unordered layer pair. */
    enum class SimulationPairResponse : std::uint8_t {
        Ignore,
        Overlap,
        Block,
        Count
    };

    /** @brief Project-authored response of one collision profile to one query channel. */
    enum class CollisionQueryResponse : std::uint8_t {
        Ignore,
        Overlap,
        Block,
        Count
    };

    /** @brief Stable collision-layer definition stored by the Project domain. */
    struct CollisionLayerDefinition final {
        CollisionLayerId id;
        bool enabled{true};
        bool admitsStatic{true};
        bool admitsKinematic{true};
        bool admitsDynamic{true};
        bool admitsOverlap{true};
    };

    /** @brief One explicit response for an unordered pair of enabled collision layers. */
    struct CollisionPairDefinition final {
        CollisionLayerId first;
        CollisionLayerId second;
        SimulationPairResponse response{SimulationPairResponse::Ignore};
    };

    /** @brief Stable project query-channel definition. */
    struct CollisionQueryChannelDefinition final {
        PhysicsQueryChannelId id;
        bool enabled{true};
    };

    /** @brief One complete profile entry for a query channel. */
    struct CollisionProfileQueryResponse final {
        PhysicsQueryChannelId channel;
        CollisionQueryResponse response{CollisionQueryResponse::Ignore};
    };

    /** @brief Reusable collision profile stored by stable identity. */
    struct CollisionProfileDefinition final {
        CollisionProfileId id;
        CollisionLayerId layer;
        bool simulationEnabled{true};
        bool queryEnabled{true};
        bool lifecycleEventsEnabled{true};
        std::vector<CollisionProfileQueryResponse> queryResponses;
    };

    /** @brief Complete serialization-ready project collision document. */
    struct ProjectCollisionSchema final {
        std::uint32_t schemaVersion{1};
        std::uint64_t requiredFeatureBits{};
        CollisionProfileId defaultProfile;
        std::vector<CollisionLayerDefinition> layers;
        std::vector<CollisionPairDefinition> pairs;
        std::vector<CollisionQueryChannelDefinition> queryChannels;
        std::vector<CollisionProfileDefinition> profiles;
    };

    /** @brief Immutable, canonical, identity-sorted filter data safe for per-world compilation. */
    class NormalizedCollisionSchema final {
    public:
        NormalizedCollisionSchema(const NormalizedCollisionSchema &) = delete;
        NormalizedCollisionSchema &operator=(const NormalizedCollisionSchema &) = delete;
        NormalizedCollisionSchema(NormalizedCollisionSchema &&) noexcept = default;
        NormalizedCollisionSchema &operator=(NormalizedCollisionSchema &&) = delete;

        /**
         * @brief Validates a complete authored schema and compiles canonical immutable filter data.
         * @param authored Project-owned serialization representation; input order is not significant.
         * @return Normalized schema or a stable descriptor/capacity error without partial publication.
         */
        [[nodiscard]] static Result<NormalizedCollisionSchema> Create(const ProjectCollisionSchema &authored);

        /** @brief Returns identity-sorted layer definitions. */
        [[nodiscard]] std::span<const CollisionLayerDefinition> Layers() const noexcept;
        /** @brief Returns canonical unordered pair definitions. */
        [[nodiscard]] std::span<const CollisionPairDefinition> Pairs() const noexcept;
        /** @brief Returns identity-sorted query channels. */
        [[nodiscard]] std::span<const CollisionQueryChannelDefinition> QueryChannels() const noexcept;
        /** @brief Returns identity-sorted profiles with identity-sorted complete responses. */
        [[nodiscard]] std::span<const CollisionProfileDefinition> Profiles() const noexcept;
        /** @brief Returns the required default profile identity. */
        [[nodiscard]] CollisionProfileId DefaultProfile() const noexcept;
        /** @brief Returns a deterministic fingerprint of semantic fields only. */
        [[nodiscard]] std::uint64_t SemanticFingerprint() const noexcept;

        /** @brief Resolves the response for an exact layer pair without fallback. */
        [[nodiscard]] Result<SimulationPairResponse> ResolvePair(CollisionLayerId first, CollisionLayerId second) const;
        /** @brief Resolves an exact profile without substituting the default. */
        [[nodiscard]] Result<const CollisionProfileDefinition *> ResolveProfile(CollisionProfileId id) const;
        /** @brief Resolves an explicit profile eligible for static sensor simulation and lifecycle events.
         * @param id Exact project profile; no default substitution.
         * @return Borrowed profile or a typed invalid/unknown profile or layer diagnostic; no allocation or native work.
         */
        [[nodiscard]] Result<const CollisionProfileDefinition *> ResolveStaticSensorProfile(CollisionProfileId id) const;
        /** @brief Resolves one profile/channel response without fallback. */
        [[nodiscard]] Result<CollisionQueryResponse> ResolveQuery(CollisionProfileId profile, PhysicsQueryChannelId channel) const;

    private:
        NormalizedCollisionSchema(CollisionProfileId defaultProfile, std::vector<CollisionLayerDefinition> layers,
                                  std::vector<CollisionPairDefinition> pairs, std::vector<CollisionQueryChannelDefinition> channels,
                                  std::vector<CollisionProfileDefinition> profiles, std::uint64_t fingerprint);

        CollisionProfileId defaultProfile_;
        std::vector<CollisionLayerDefinition> layers_;
        std::vector<CollisionPairDefinition> pairs_;
        std::vector<CollisionQueryChannelDefinition> channels_;
        std::vector<CollisionProfileDefinition> profiles_;
        std::uint64_t fingerprint_{};
    };
}  // namespace Horo::Physics
