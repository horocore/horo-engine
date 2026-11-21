#pragma once

/**
 * @file EnvironmentQueryGenerators.h
 * @brief Bounded tactical candidate generation over captured inputs.
 */

#include "Horo/AI/AIScenePerceptionSource.h"
#include "Horo/AI/EnvironmentQueryAuthoring.h"
#include "Horo/AI/EnvironmentQueryContexts.h"
#include "Horo/AI/EnvironmentQueryScoring.h"

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

        /** @brief Checks provenance, the hard item ceiling and destination size. @return True for an admitted output. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Bounded generation evidence. Only the first itemCount entries are published. */
    struct QueryGenerationResult final {
        QueryGeneratorProvenance provenance;
        std::size_t itemCount{};
        std::size_t invalidCoordinateCount{}; /**< Offsets outside the canonical world coordinate range. */
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

    /**
     * @brief Captures the registered native point-lattice provider and existing built-in context descriptors.
     * @return Immutable validated registry or the owning descriptor diagnostic.
     * @details Composition owns this registry. Capturing metadata performs no Scene access or execution.
     */
    [[nodiscard]] Result<QuerySchemaRegistry> CaptureBuiltinQuerySchema();

    /** @brief Adds host-admitted point context descriptors to the built-in generator and distance test choices.
     * @param locationContexts Additional descriptors whose paired providers return canonical world coordinates.
     * @return The inert combined registry or a typed identity, version, or capacity error. No provider is invoked.
     */
    [[nodiscard]] Result<QuerySchemaRegistry> CaptureBuiltinQuerySchema(std::span<const QueryContextDescriptor> locationContexts);

    /**
     * @brief Constructs one initial source through the installed point-lattice provider's typed wire contract.
     * @param query Stable authoring-issued query identity. @param stage Stable stage identity.
     * @param result Stable result schema identity. @param displayName Authored UTF-8 query label.
     * @param lattice Explicit typed user-selected grid or ring settings. @param maximumResults Maximum published ranked results.
     * @return A validated immutable source, or a typed lattice/schema error. Source capture performs no reads or writes.
     */
    [[nodiscard]] Result<EnvironmentQueryAsset> CreateBuiltinLatticeQuery(QueryId query, QueryStageId stage, QueryResultSchemaId result,
                                                                          std::string displayName, const QueryPointLattice &lattice,
                                                                          std::uint32_t maximumResults);

    /**
     * @brief Dispatches an admitted point-lattice stage through its exact native provider and current query context.
     * @param plan Immutable compiled query. @param contexts Context captured for the same query revision.
     * @param output Caller-owned destination and exact registered provenance.
     * @return Generation evidence or a typed incompatible/stale/budget failure, with no partial output on failure.
     */
    [[nodiscard]] Result<QueryGenerationResult> GenerateBuiltinQueryCandidates(const EnvironmentQueryPlan &plan,
                                                                               const QueryContextSnapshot &contexts,
                                                                               const QueryGeneratorOutput &output);

    /** @brief Appends a point-distance stage to a native lattice source in semantic execution order.
     * @param lattice Native lattice with zero or more native distance stages. @param stage Fresh stable test stage identity.
     * @param settings Explicit radius and optional score. @return Detached complete source or typed refusal.
     * @post Existing stages retain their identities, source order and execution order.
     */
    [[nodiscard]] Result<EnvironmentQueryAsset> AddBuiltinPointDistanceTest(const EnvironmentQueryAsset &lattice, QueryStageId stage,
                                                                            QueryPointDistanceTest settings);

    /** @brief Adds one blocking-geometry visibility filter to a native lattice with no tests.
     * @param lattice Exact native point generator. @param stage Fresh test identity.
     * @param channel Canonical 16-byte identity of the project collision channel.
     * @return Detached query source or typed validation error.
     */
    [[nodiscard]] Result<EnvironmentQueryAsset> AddBuiltinPointVisibilityTest(const EnvironmentQueryAsset &lattice, QueryStageId stage,
                                                                              std::array<std::uint8_t, 16> channel);

    /** @brief Validated native line-of-sight plan for a bounded Physics owner batch. */
    struct BuiltinPointVisibilityPlan final {
        QueryPointLattice lattice;
        QueryGeneratorProvenance generator;
        QueryStageId test;
        std::array<std::uint8_t, 16> channel;
    };

    /** @brief Checks for the native GeometryLineOfSight test in a compiled plan. */
    [[nodiscard]] bool UsesBuiltinPointVisibility(const EnvironmentQueryPlan &plan) noexcept;

    /** @brief Validates exact native lattice and visibility descriptors before provider access. */
    [[nodiscard]] Result<BuiltinPointVisibilityPlan> CaptureBuiltinPointVisibility(const EnvironmentQueryPlan &plan,
                                                                                   const QuerySchemaRegistry &registry,
                                                                                   const QueryContextSnapshot &contexts);

    /** @brief Removes a native point-distance stage while retaining all other source data.
     * @param lattice Detached source containing the stage. @param stage Stable identity of the test to remove.
     * @return Detached source or a typed refusal when the identity is absent or selects a different stage kind.
     */
    [[nodiscard]] Result<EnvironmentQueryAsset> RemoveBuiltinPointDistanceTest(const EnvironmentQueryAsset &lattice, QueryStageId stage);

    /** @brief Decodes the exact supported native authored shape and settings.
     * @param asset Saved or draft query. @param registry Exact installed descriptor catalog.
     * @return The typed editable settings, or a refusal that preserves unsupported source bytes.
     */
    [[nodiscard]] Result<BuiltinPointQuerySettings> ReadBuiltinPointQuerySettings(const EnvironmentQueryAsset &asset,
                                                                                  const QuerySchemaRegistry &registry);

    /** @brief Updates values on the admitted native source while retaining every authored identity and stage order.
     * @param asset Original typed source. @param registry Exact installed descriptor catalog.
     * @param settings Replacement values with exactly the original distance-stage identities in execution order.
     * @return Complete validated source or typed refusal without modifying the original.
     */
    [[nodiscard]] Result<EnvironmentQueryAsset> EditBuiltinPointQuerySettings(const EnvironmentQueryAsset &asset,
                                                                              const QuerySchemaRegistry &registry,
                                                                              const BuiltinPointQuerySettings &settings);

    /** @brief Prepares the exact installed point-distance test without invoking a provider callback. */
    class BuiltinPointDistanceQuery final {
    public:
        /** @brief Resolves one compiled stage against the exact registered native descriptor.
         * @param stage Compiled test. @param registry Same registry used to compile the query.
         * @return Typed immutable kernel or refusal for unsupported descriptors/settings.
         */
        [[nodiscard]] static Result<BuiltinPointDistanceQuery> Capture(const QueryPlanStage &stage, const QuerySchemaRegistry &registry);
        /** @brief Returns the canonical filter and optional scoring policy for this stage. @return Scoring rule. */
        [[nodiscard]] QueryScoreRule Rule() const;

        /** @brief Returns the exact decoded authored policy. @return Immutable distance settings. */
        [[nodiscard]] QueryPointDistanceTest Settings() const noexcept {
            return settings_;
        }

        /** @brief Runs the actual distance kernel on two captured canonical points.
         * @param center Querier position. @param point Candidate position. @return Typed filter and metric evidence.
         */
        [[nodiscard]] Result<QueryTestSample> Evaluate(Math::WorldCoordinate64 center, Math::WorldCoordinate64 point) const;

    private:
        BuiltinPointDistanceQuery(QueryStageId stage, QueryPointDistanceTest settings) : stage_(stage), settings_(settings) {}

        QueryStageId stage_;
        QueryPointDistanceTest settings_;
    };

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
