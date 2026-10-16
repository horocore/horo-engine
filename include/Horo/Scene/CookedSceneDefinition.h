#pragma once

/** @file CookedSceneDefinition.h
 * @brief Bounded source-free scene payload codec; asset envelopes and generation publication remain Assets-owned.
 */
#include "Horo/Assets/AssetId.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"

#include <string_view>

namespace Horo::SceneCook {
    /** @brief Required wire version including typed Character components; v1/v2 content requires recook. */
    inline constexpr std::string_view CookedSceneFormat = "horo.scene.runtime.v3";

    /** @brief Derives the canonical stable Scene definition identity from its owning asset.
     * @param asset Stable nonzero Scene asset identity verified by the host's asset authority.
     * @return Low eight SHA-256 digest bytes in little-endian order, with zero mapped to one.
     * @details This preserves the existing cooker identity algorithm. It is a compact identity,
     * not a collision-proof digest; hosts must reject containing-Scene collisions before publication.
     * It never allocates a runtime Scene instance identity or validates an asset envelope.
     */
    [[nodiscard]] Runtime::SceneDefinitionId SceneDefinitionIdForAsset(Assets::AssetId asset);

    /** @brief Positive immutable load/cook limits for one complete scene payload. */
    struct CookedSceneLimits final {
        std::size_t maximumEntities{100'000};
        std::size_t maximumDependencies{16'384};
        std::size_t maximumBytes{16U * 1024U * 1024U};
    };

    namespace SceneCookErrors {
        /** @brief Malformed, noncanonical or incompatible scene payload or identity. */
        extern const ErrorCodeDescriptor Invalid;
        /** @brief A required runtime component has no supported portable scene encoding. */
        extern const ErrorCodeDescriptor Unsupported;
        /** @brief Complete scene exceeds captured entity, dependency or byte limits. */
        extern const ErrorCodeDescriptor TooLarge;
    }  // namespace SceneCookErrors

    /**
     * @brief Encodes a complete validated runtime definition using the current typed scene payload schema.
     * @param definition Detached source-free runtime definition after all required placements succeeded.
     * @param limits Captured positive entity, dependency and payload-byte bounds.
     * @return Canonical owned bytes or typed capacity/unsupported-schema failure.
     * @details No prefab references, names, editor state, source paths or registry lookups enter the payload.
     * Core component scalar codecs are shared with Scene source; the outer runtime wire contract is separate.
     * Unsupported required modes fail rather than dropping data. Asset envelope integrity is a separate required layer.
     */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodeCookedSceneDefinition(const Runtime::RuntimeSceneDefinition &definition,
                                                                                const CookedSceneLimits &limits = {});

    /**
     * @brief Decodes only canonical source-free scene bytes into one validated immutable runtime definition.
     * @param bytes Exact payload already verified by the Assets envelope owner.
     * @param expectedScene Stable scene identity captured by the loader.
     * @param expectedRevision Exact cooked content revision, never a live runtime generation.
     * @param limits Captured positive storage bounds checked before copying entity/dependency data.
     * @return Complete immutable definition or typed malformed, foreign, unsupported or capacity failure.
     * @details Rejects raw prefab sections, duplicate/unknown fields, excessive nesting and noncanonical bytes.
     * No source parsing, migration, prefab resolution, backend activation or filesystem access occurs.
     */
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> DecodeCookedSceneDefinition(std::span<const std::uint8_t> bytes,
                                                                                      Runtime::SceneDefinitionId expectedScene,
                                                                                      Runtime::SceneDefinitionRevision expectedRevision,
                                                                                      const CookedSceneLimits &limits = {});

    /** @brief Admits a verified runtime asset's embedded durable revision through the same canonical Scene decoder.
     * @param bytes Exact source-free payload after the host verifies the AssetCook envelope identity, type, target and digests.
     * @param expectedScene Stable Scene identity selected by that host; a foreign embedded identity is rejected.
     * @param limits Positive storage bounds checked before copying typed content.
     * @return Fully validated canonical definition retaining its embedded nonzero durable revision, or the original decode failure.
     * @pre The application authorized the project/asset and verified its envelope; this function grants neither trust nor freshness.
     * @details An embedded revision identifies this immutable payload, not the current source or an active runtime generation.
     * Callers requiring a particular source/cook revision must use DecodeCookedSceneDefinition with that exact expected revision.
     */
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> DecodeCookedSceneWithEmbeddedRevision(std::span<const std::uint8_t> bytes,
                                                                                                Runtime::SceneDefinitionId expectedScene,
                                                                                                const CookedSceneLimits &limits = {});
}  // namespace Horo::SceneCook
