#pragma once

/**
 * @file AssetChunkPlan.h
 * @brief Validated deterministic grouping of cooked asset identities for release archives.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Sha256.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets {
    /** @brief Portable identity of one release content chunk. */
    class AssetChunkId final {
    public:
        /** @brief Parses a bounded lowercase chunk identity. @param text Candidate identity. @return Parsed ID or typed failure. */
        [[nodiscard]] static Result<AssetChunkId> Parse(std::string_view text);
        /** @brief Returns canonical text. @return Borrowed immutable identity. */
        [[nodiscard]] const std::string &Value() const noexcept;
        [[nodiscard]] auto operator<=>(const AssetChunkId &) const = default;

    private:
        explicit AssetChunkId(std::string value);
        std::string value_;
    };

    /** @brief Distribution role of a chunk; admission policy does not infer it from its name. */
    enum class AssetChunkKind : std::uint8_t {
        Base,
        Optional,
        Language,
        Dlc,
        DedicatedServer,
    };

    /** @brief Exact authored membership and dependency intent for one chunk. */
    struct AssetChunkDefinition {
        AssetChunkId id;
        AssetChunkKind kind{AssetChunkKind::Base};
        std::vector<AssetId> assets;
        std::vector<AssetChunkId> dependencies;
        std::int32_t mountPriority{};
        std::optional<Sha256Digest> requiredBaseManifest; /**< Mandatory for DLC, optional for other non-base content. */
    };

    /** @brief Finite validation policy for one release content plan. */
    struct AssetChunkPlanLimits {
        std::size_t maximumChunks{1024U};
        std::size_t maximumAssets{100'000U};
        std::size_t maximumDependenciesPerChunk{128U};
    };

    /** @brief Canonically sorted, duplicate-free chunk graph admitted for packaging. */
    class AssetChunkPlan final {
    public:
        /**
         * @brief Validates exact membership, dependency closure, release roles, and graph acyclicity.
         * @param definitions Authored chunks, in any order.
         * @param limits Host-owned finite bounds.
         * @return Immutable canonical plan or a typed failure; no partial plan escapes.
         */
        [[nodiscard]] static Result<AssetChunkPlan> Create(std::span<const AssetChunkDefinition> definitions,
                                                           const AssetChunkPlanLimits &limits = {});
        /** @brief Returns chunks in stable ID order. @return Borrowed immutable definitions. */
        [[nodiscard]] std::span<const AssetChunkDefinition> Chunks() const noexcept;

    private:
        explicit AssetChunkPlan(std::vector<AssetChunkDefinition> chunks);
        std::vector<AssetChunkDefinition> chunks_;
    };

    /**
     * @brief Resolves an exact installed selection into deterministic dependency-first mount order.
     * @param plan Validated release chunk graph.
     * @param selected Exact chunks to mount, including one base chunk.
     * @param baseManifest Digest of the verified base release manifest.
     * @return Ordered IDs or failure for missing dependencies, duplicate selections, incompatible DLC, or invalid mount priority.
     * @note The caller must verify package signatures and archive bytes before using this plan for mounting.
     */
    [[nodiscard]] Result<std::vector<AssetChunkId>> ResolveAssetChunkMountOrder(const AssetChunkPlan &plan,
                                                                                std::span<const AssetChunkId> selected,
                                                                                const Sha256Digest &baseManifest);

    /**
     * @brief Plans removal without allowing a remaining chunk to lose a dependency or its base.
     * @param plan Validated release chunk graph.
     * @param installed Exact currently installed selection.
     * @param removed Chunk requested for removal.
     * @param baseManifest Digest of the verified base release manifest.
     * @return Dependency-first order of the remaining chunks; empty only when the last base chunk is removed.
     */
    [[nodiscard]] Result<std::vector<AssetChunkId>> PlanAssetChunkRemoval(const AssetChunkPlan &plan,
                                                                          std::span<const AssetChunkId> installed,
                                                                          const AssetChunkId &removed, const Sha256Digest &baseManifest);
}  // namespace Horo::Assets
