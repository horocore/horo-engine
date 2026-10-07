#pragma once

/** @file CookedSceneDefinition.h
 * @brief Bounded source-free scene payload codec; asset envelopes and generation publication remain Assets-owned.
 */
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"

namespace Horo::SceneCook {
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
     * @brief Encodes a complete validated runtime definition using scene payload schema 1.
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
}  // namespace Horo::SceneCook
