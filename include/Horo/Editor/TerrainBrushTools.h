#pragma once

/** @file TerrainBrushTools.h @brief Bounded deterministic source-space brush planning, independent of presentation. */
#include "Horo/Editor/TerrainAuthoringDocument.h"

#include <span>

namespace Horo::Editor {
    /** @brief Explicit brush semantics; unsupported channels are rejected rather than initialized implicitly. */
    enum class TerrainBrushMode : std::uint8_t {
        Raise,
        Lower,
        Smooth,
        Flatten,
        Noise,
        Paint,
        Carve,
        Restore
    };

    /** @brief One source-space dab; sample coordinates may be fractional, including at dataset edges. */
    struct TerrainBrushDab final {
        double x{}, z{};
    };

    /** @brief Captured gesture settings; strength is meters for raise/lower/noise and [0,1] for blending. */
    struct TerrainBrushSettings final {
        TerrainBrushMode mode{TerrainBrushMode::Raise};
        double radiusMeters{1.0};
        float strength{1.0F}, flattenHeight{};
        std::uint32_t noiseSeed{};
        std::uint8_t layer{}; /**< Exact canonical authored layer order, never sorted by weight. */
    };

    /** @brief Independent finite planning ceilings, including repeated reads and smooth aprons. */
    struct TerrainBrushLimits final {
        std::size_t maximumDabs{1024}, maximumSamples{262144}, maximumReadVisits{4194304};
        std::size_t maximumOwnedBytes{16 * 1024 * 1024};
    };

    /** @brief Builds one exact bounded transaction without copying or mutating the entire document.
     * @param document Sole canonical owner, borrowed only during this call.
     * @param settings Frozen gesture semantics. Carve requires an existing authored per-vertex hole mask and finite strength in [0,1].
     * Carve uses physical radial linear falloff: clamp(strength * falloff, 0, 1) >= 0.5 sets a hole byte to one;
     * lower influence preserves the existing bit. This categorical threshold is the planner's bounded policy, not a format rule.
     * Dabs retain caller order; no other channel is initialized or written. Restore remains unsupported.
     * @param dabs Ordered source-space dabs; UI event timing never changes an already captured command.
     * @param operation Host-issued operation identity.
     * @param limits Explicit finite planner ceilings, additional to document/history admission.
     * @param cancellation Cooperative observer checked before and during work.
     * @return Exact affected patch command or typed denial; no canonical/history change.
     */
    [[nodiscard]] Result<TerrainEditOperation> PlanTerrainBrush(const TerrainAuthoringDocument &document,
                                                                const TerrainBrushSettings &settings, std::span<const TerrainBrushDab> dabs,
                                                                TerrainEditOperationId operation, const TerrainBrushLimits &limits = {},
                                                                const CancellationToken &cancellation = {});
}  // namespace Horo::Editor
