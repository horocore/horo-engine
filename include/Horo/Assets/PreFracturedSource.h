#pragma once

/**
 * @file PreFracturedSource.h
 * @brief Detached, backend-neutral FBX mesh-node geometry for fracture authoring.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets {
    /** @brief Version of the canonical FBX mesh-node normalization contract. */
    inline constexpr std::uint32_t CurrentPreFracturedSourceSchemaVersion = 1;

    /** @brief One source mesh occurrence, retaining hierarchy and material attribution. */
    struct PreFracturedSourceNode final {
        std::string name;                            /**< Authored FBX node name, not a runtime identity. */
        std::string sourcePath;                      /**< Diagnostic node path within this source parse, not an identity. */
        std::optional<std::uint32_t> parent;         /**< Nearest mesh ancestor in nodes, if any. */
        std::array<double, 12> geometryToWorld{};    /**< Canonical affine transform before baking positions. */
        std::vector<std::array<float, 3>> positions; /**< Canonical world-space positions. */
        std::vector<std::uint32_t> triangleIndices;  /**< Local indices in triangle order. */
        std::vector<std::string> triangleMaterials;  /**< Material name per triangle; empty means missing. */
    };

    /** @brief Detached immutable-input snapshot; callers own all copied data. */
    struct PreFracturedSource final {
        std::uint32_t schemaVersion{CurrentPreFracturedSourceSchemaVersion}; /**< Exact normalization schema. */
        std::string sourceName;                                              /**< Caller-supplied diagnostic source label. */
        std::vector<PreFracturedSourceNode> nodes;                           /**< Mesh occurrences in source traversal order. */
    };

    /**
     * @brief Parses an ASCII or binary FBX scene without publishing an asset or retaining parser state.
     * @param bytes Complete borrowed source bytes, at most 512 MiB.
     * @param sourceName User-visible source label retained only for diagnostics.
     * @param cancellation Cooperative cancellation observed during node, vertex, and face traversal.
     * @return Detached normalized source, or a typed error with source-context diagnostics.
     */
    [[nodiscard]] Result<PreFracturedSource> ParsePreFracturedFbx(std::span<const std::uint8_t> bytes, std::string_view sourceName,
                                                                  const CancellationToken &cancellation);
}  // namespace Horo::Assets
