#pragma once

/** @file PCGPointGeneration.h
 * @brief Typed authored settings and stable sample identities for deterministic point generation. */

#include "Horo/Foundation/Result.h"
#include "Horo/PCG/PCGIdentity.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Horo::PCG {
    /** @brief Unsigned-integer point attribute containing the canonical source sample identity.
     * Generation assigns a semantic lattice-cell or attempted-sample identity, before acceptance or compaction.
     * Filtering and merging retain this column and all four root columns. The composite identifies a sample in its exact canonical root. */
    inline constexpr std::string_view PCGSourceSampleAttribute = "pcg.source_sample_id";

    /** @brief Four unsigned-integer attributes containing network-order words of existing PCGProvenance::Key.
     * The schema-1 identity is the complete (root words, sample) tuple. Root bytes are copied, never rehashed.
     * New generation requires all five fields; Grid v1 permits either all five fields or none. */
    inline constexpr std::array<std::string_view, 4> PCGSourceRootAttributes{"pcg.source_root_0", "pcg.source_root_1", "pcg.source_root_2",
                                                                             "pcg.source_root_3"};

    /** @brief Existing version-1 SnapshotGrid settings; lattice traversal is canonical x, then y, then z. */
    struct PCGGridGenerationSettings final {
        SpatialElementId grid{}; /**< Exact immutable provider grid identity. */
    };

    /** @brief One reproducibly jittered sample per canonical provider-grid lattice cell. */
    struct PCGJitterGenerationSettings final {
        SpatialElementId grid{}; /**< Exact immutable provider grid identity. */
        double fraction{1.0};    /**< Finite fraction in [0,1] of each half-spacing jitter window. */
    };

    /** @brief One reproducible sample in every stratum of one immutable box volume. */
    struct PCGStratifiedGenerationSettings final {
        SpatialElementId volume{};                    /**< Exact immutable box-volume identity; other volume kinds are unsupported. */
        std::array<std::uint32_t, 3> strata{1, 1, 1}; /**< Positive x/y/z counts, admitted with checked arithmetic. */
    };

    /** @brief Bounded rejection scatter with separation measured between the final stored float coordinates.
     * The operation either produces the requested count or fails; it never returns a partial successful point set. */
    struct PCGPoissonGenerationSettings final {
        SpatialElementId volume{};   /**< Exact immutable box-volume identity. */
        std::uint32_t count{};       /**< Exact requested point count; zero is an explicitly empty result. */
        std::uint32_t attempts{};    /**< Complete attempted-sample ceiling, including accepted samples. */
        double minimumDistance{1.0}; /**< Positive finite minimum pairwise Euclidean separation in snapshot units. */
    };

    /** @brief Encodes the existing SnapshotGrid version-1 payload without changing its schema.
     * @param settings Exact typed provider-grid identity.
     * @return Canonical eight-byte settings or a typed identity/allocation failure. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodePCGPointGenerationSettings(const PCGGridGenerationSettings &settings);

    /** @brief Encodes version-1 jitter settings with canonical network-order values.
     * @param settings Exact grid identity and finite jitter fraction.
     * @return Canonical node payload or a typed malformed/allocation failure. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodePCGPointGenerationSettings(const PCGJitterGenerationSettings &settings);

    /** @brief Encodes version-1 stratified settings with canonical network-order values.
     * @param settings Exact volume identity and positive stratum dimensions.
     * @return Canonical node payload or a typed malformed/allocation failure. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodePCGPointGenerationSettings(const PCGStratifiedGenerationSettings &settings);

    /** @brief Encodes version-1 bounded Poisson-style scatter settings.
     * @param settings Exact volume, count, complete attempt budget and positive separation.
     * @return Canonical node payload or a typed malformed/allocation failure; density and budget feasibility are evaluation diagnostics. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodePCGPointGenerationSettings(const PCGPoissonGenerationSettings &settings);
}  // namespace Horo::PCG
