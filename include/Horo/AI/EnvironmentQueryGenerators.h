#pragma once

/**
 * @file EnvironmentQueryGenerators.h
 * @brief Bounded tactical candidate generation over captured inputs.
 */

#include "Horo/AI/AIScenePerceptionSource.h"
#include "Horo/AI/EnvironmentQueryContexts.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace Horo::AI {
    /** @brief Identifies the generator stage and the exact source publication used for a candidate batch. */
    struct QueryGeneratorProvenance final {
        QueryStageId stage;
        QueryGeneratorId generator;
        QueryProviderId provider;
        std::uint64_t revision{};
    };

    /** @brief One generated point or weak actor reference. Its index in the result is its stable candidate index. */
    using QueryGeneratedItem = std::variant<Math::WorldCoordinate64, Runtime::EntityRef>;

    /** @brief Owner-supplied output storage. A failure leaves storage unchanged. */
    struct QueryGeneratorOutput final {
        QueryGeneratorProvenance provenance;
        std::size_t maximumItems{};
        std::span<QueryGeneratedItem> items;
    };

    /** @brief Bounded generation evidence. Only the first itemCount entries are published. */
    struct QueryGenerationResult final {
        QueryGeneratorProvenance provenance;
        std::size_t itemCount{};
        std::size_t invalidCoordinateCount{}; /**< Offsets outside the canonical world coordinate range. */
    };

    /** @brief Horizontal square or annulus sampled on an exact millimeter lattice. */
    struct QueryPointLattice final {
        std::uint32_t stepMillimeters{};
        std::uint32_t outerSteps{};
        std::optional<std::uint32_t> innerSteps; /**< Absent for a square; present for a closed annulus. */
    };

    /**
     * @brief Emits grid or ring candidates in increasing Z, then X offset order.
     * @param contexts Captured execution context with a position matching the supplied revision.
     * @param center Exact context ID of the canonical center.
     * @param lattice Sampling dimensions and density.
     * @param output Destination and admitted maximum item count. The bounding square must fit the hard work limit.
     * @return Batch evidence or typed invalid, missing, stale or limit failure.
     */
    [[nodiscard]] Result<QueryGenerationResult> GenerateQueryLattice(const QueryContextSnapshot &contexts, QueryContextId center,
                                                                     const QueryPointLattice &lattice, const QueryGeneratorOutput &output);

    /**
     * @brief Emits captured authored or Navigation-projected points in the provider's declared canonical order.
     * @param points Bounded canonical points already captured by the owning provider. No provider work is invoked.
     * @param output Destination, admitted maximum item count and the exact input revision.
     * @return Batch evidence or typed invalid or limit failure.
     */
    [[nodiscard]] Result<QueryGenerationResult> GenerateQueryPoints(std::span<const Math::WorldCoordinate64> points,
                                                                    const QueryGeneratorOutput &output);

    /** @brief Borrows the recipient's current authorized perception publication on the scene owner lane. */
    struct QueryPerceivedEntitySource final {
        const Runtime::RuntimeSceneView &scene;
        const PerceptionPublication &publication;
        const PerceptionSpatialBroadphase &filtering;
        const BlackboardInstance &blackboard;
    };

    /**
     * @brief Emits distinct actors from the exact authorized perception revision in entity slot/generation order.
     * @param source Current recipient publication and the owners required to validate it.
     * @param output Destination and exact perception revision.
     * @return Batch evidence or typed stale, invalid or limit failure.
     */
    [[nodiscard]] Result<QueryGenerationResult> GenerateQueryPerceivedEntities(const QueryPerceivedEntitySource &source,
                                                                               const QueryGeneratorOutput &output);
}  // namespace Horo::AI
