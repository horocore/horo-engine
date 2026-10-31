#pragma once

/** @file PrefabDebugSnapshot.h
 * @brief Bounded immutable inspection of canonical prefab graph, expansion and override evidence.
 */
#include "Horo/Prefab/PrefabValueOverrides.h"

#include <optional>
#include <span>
#include <vector>

namespace Horo::Prefab {
    /** @brief Tooling-only capture ceilings, independent of project content admission policy. */
    struct PrefabDebugCaptureLimits final {
        static constexpr std::size_t MaximumEntries = 4096;
        std::size_t maximumEntries{64}; /**< Shared retained-row/work ceiling across all sections; zero captures totals only. */
    };

    /** @brief Exact input cardinality and explicitly omitted rows for one snapshot section. */
    struct PrefabDebugSection final {
        std::size_t available{};
        std::size_t retained{};

        /** @brief Returns omitted rows without implying missing source content. @return Exact omitted count. */
        [[nodiscard]] std::size_t Omitted() const noexcept {
            return available - retained;
        }
    };

    /** @brief Payload-free provenance of one expanded object; no mutable document or runtime entity. */
    struct PrefabDebugObject final {
        Assets::AssetId sourcePrefab;
        ExpandedPrefabObjectKey key;
        std::optional<ExpandedPrefabObjectKey> parent;
        std::size_t componentCount{};
    };

    /** @brief Payload-free value provenance; values/defaults and executable schema metadata are excluded. */
    struct PrefabDebugValue final {
        PrefabValueOverrideTarget target;
        PrefabValueOrigin origin{PrefabValueOrigin::Source};
        Assets::AssetId sourcePrefab;
    };

    /** @brief Original intent classification, independent of display labels or payload encoding. */
    enum class PrefabDebugOverrideKind : std::uint8_t {
        AssignValue,
        AddComponent,
        RemoveComponent
    };
    /** @brief Resolution state of one retained intent, including preserved unresolved records. */
    enum class PrefabDebugOverrideState : std::uint8_t {
        Active,
        Conflict,
        Orphan
    };

    /** @brief Exact target and state without retaining arbitrary values or component bytes. */
    struct PrefabDebugOverride final {
        std::variant<PrefabValueOverrideTarget, PrefabComponentOverrideTarget> target;
        PrefabDebugOverrideKind kind{PrefabDebugOverrideKind::AssignValue};
        PrefabDebugOverrideState state{PrefabDebugOverrideState::Active};
    };

    /** @brief Constant-time exact override counts, including topology conflicts and orphans. */
    struct PrefabDebugOverrides final {
        bool supplied{};
        PrefabSourceRevision authoredAgainst;
        Sha256Digest schemaRevision;
        std::size_t assignments{};
        std::size_t conflicts{};
        std::size_t orphans{};
        std::size_t componentOperations{};
        std::size_t componentConflicts{};
        std::size_t componentOrphans{};
        std::size_t redundantRecords{};
    };

    /**
     * @brief Immutable, owned, backend-neutral debug publication from existing immutable operation results.
     * @details Capture is tooling/load-time work over pinned results on a worker, never a live-owner query,
     * lock, source read or simulation callback. Sections retain deterministic prefixes in nodes/edges/objects/
     * override/provenance order under one shared row budget. No payload/name/value bytes are copied or hashed.
     * Configured policy is reported separately from observed input counts. Parser bytes/depth, expansion work
     * consumed, cook bytes and runtime usage are not observed by this transformation and are never inferred.
     * Retiring sources, schemas, documents or runtime worlds cannot invalidate a published snapshot.
     */
    class PrefabDebugSnapshot final {
    public:
        /** @brief Returns the stable inspected root. @return Path-independent AssetId. */
        [[nodiscard]] Assets::AssetId RootAsset() const noexcept {
            return root_;
        }

        /** @brief Returns the pinned source revision. @return Exact canonical source evidence. */
        [[nodiscard]] const PrefabSourceRevision &SourceRevision() const noexcept {
            return source_;
        }

        /** @brief Returns the pinned registry publication. @return Exact registry revision. */
        [[nodiscard]] Assets::AssetRegistryRevision RegistryRevision() const noexcept {
            return registry_;
        }

        /** @brief Returns configured source/graph/expansion/override/cook/runtime ceilings. @return Captured policy, not usage. */
        [[nodiscard]] const PrefabProjectPolicy &Policy() const noexcept {
            return policy_;
        }

        /** @brief Returns configured derived expansion work capacity. @return Ceiling, not measured consumption. */
        [[nodiscard]] std::size_t ExpansionWorkCeiling() const noexcept {
            return expansionWorkCeiling_;
        }

        /** @brief Returns the parser's encoded-byte ceiling. @return Canonical engine hard ceiling. */
        [[nodiscard]] static constexpr std::size_t ParserByteCeiling() noexcept {
            return PrefabHardLimits::SourceDocumentBytes;
        }

        /** @brief Returns the parser's nesting ceiling. @return Canonical engine hard ceiling. */
        [[nodiscard]] static constexpr std::size_t ParserDepthCeiling() noexcept {
            return PrefabHardLimits::SourceJsonDepth;
        }

        /** @brief Returns graph-node capture coverage. @return Exact totals and retained count. */
        [[nodiscard]] PrefabDebugSection NodeCoverage() const noexcept {
            return nodeCoverage_;
        }

        /** @brief Returns graph-edge capture coverage. @return Exact totals and retained count. */
        [[nodiscard]] PrefabDebugSection EdgeCoverage() const noexcept {
            return edgeCoverage_;
        }

        /** @brief Returns expanded-object capture coverage. @return Exact totals and retained count. */
        [[nodiscard]] PrefabDebugSection ObjectCoverage() const noexcept {
            return objectCoverage_;
        }

        /** @brief Returns value-provenance capture coverage. @return Exact totals and retained count. */
        [[nodiscard]] PrefabDebugSection ValueCoverage() const noexcept {
            return valueCoverage_;
        }

        /** @brief Returns override-intent capture coverage. @return Exact totals and retained count. */
        [[nodiscard]] PrefabDebugSection OverrideCoverage() const noexcept {
            return overrideCoverage_;
        }

        /** @brief Returns whether any section omitted rows. @return True for explicit truncation. */
        [[nodiscard]] bool IsTruncated() const noexcept;

        /** @brief Returns retained canonical dependency nodes. @return Immutable bounded rows. */
        [[nodiscard]] std::span<const PrefabDependencyNode> Nodes() const noexcept {
            return nodes_;
        }

        /** @brief Returns retained canonical semantic edges. @return Immutable bounded rows. */
        [[nodiscard]] std::span<const PrefabDependencyEdge> Edges() const noexcept {
            return edges_;
        }

        /** @brief Returns retained expansion provenance. @return Immutable bounded rows. */
        [[nodiscard]] std::span<const PrefabDebugObject> Objects() const noexcept {
            return objects_;
        }

        /** @brief Returns retained override provenance. @return Immutable bounded rows. */
        [[nodiscard]] std::span<const PrefabDebugValue> Values() const noexcept {
            return values_;
        }

        /** @brief Returns retained exact override targets and states. @return Immutable payload-free intent. */
        [[nodiscard]] std::span<const PrefabDebugOverride> OverrideRecords() const noexcept {
            return overrideRecords_;
        }

        /** @brief Returns exact override counts from the supplied envelope. @return Payload-free summary. */
        [[nodiscard]] const PrefabDebugOverrides &Overrides() const noexcept {
            return overrides_;
        }

        /** @brief Returns exact candidate intent commitment. @return Zero for source-only expansion. */
        [[nodiscard]] Sha256Digest ValueOverridesDigest() const noexcept {
            return valueOverridesDigest_;
        }

    private:
        friend Result<PrefabDebugSnapshot> CapturePrefabDebugSnapshot(const EffectivePrefabCandidate &, const PrefabLimitProfile &,
                                                                      PrefabDebugCaptureLimits, const PrefabValueOverrideSet *,
                                                                      std::span<const PrefabValueProvenance>, std::size_t,
                                                                      const CancellationToken &);
        PrefabDebugSnapshot() = default;
        Assets::AssetId root_;
        PrefabSourceRevision source_;
        Assets::AssetRegistryRevision registry_;
        PrefabProjectPolicy policy_;
        std::size_t expansionWorkCeiling_{};
        Sha256Digest valueOverridesDigest_;
        PrefabDebugSection nodeCoverage_, edgeCoverage_, objectCoverage_, overrideCoverage_, valueCoverage_;
        std::vector<PrefabDependencyNode> nodes_;
        std::vector<PrefabDependencyEdge> edges_;
        std::vector<PrefabDebugObject> objects_;
        std::vector<PrefabDebugValue> values_;
        std::vector<PrefabDebugOverride> overrideRecords_;
        PrefabDebugOverrides overrides_;
    };

    /**
     * @brief Summarizes pinned canonical operation results with bounded copying and no owner interaction.
     * @param candidate Immutable source/effective expansion, including its complete reachable graph revision.
     * @param policy Exact validated policy used by the inspected operation; reported as configured ceilings.
     * @param limits Tooling-only finite shared row budget; values above MaximumEntries reject admission.
     * @param overrides Optional canonical override envelope (including unresolved intent); borrowed only during capture.
     * @param provenance Optional detached resolution provenance; values are never copied or interpreted.
     * @param redundantRecords Exact resolver-reported redundant count, zero when not supplied.
     * @param cancellation Cooperative token checked before capture and between every retained row.
     * @return Immutable owned snapshot, or canonical DiagnosticInvalid/DiagnosticBudgetExceeded on malformed input/allocation failure.
     * @details Caller keeps inputs immutable for this synchronous call and schedules it outside authoring/simulation
     * owner hot loops. Input cardinalities are read in constant time; at most maximumEntries rows are copied.
     * Each retained provenance row checks at most SourceObjectCount fixed-size object addresses. Type identity
     * strings are bounded by their canonical AssetTypeId/ComponentTypeId constructors; nested paths are fixed-size
     * and checked against the captured profile. Arbitrary input values and omitted provenance are never traversed.
     * This is an inspection projection, not source/override revalidation or instrumentation of earlier stages.
     */
    [[nodiscard]] Result<PrefabDebugSnapshot> CapturePrefabDebugSnapshot(
        const EffectivePrefabCandidate &candidate, const PrefabLimitProfile &policy, PrefabDebugCaptureLimits limits = {},
        const PrefabValueOverrideSet *overrides = nullptr, std::span<const PrefabValueProvenance> provenance = {},
        std::size_t redundantRecords = 0, const CancellationToken &cancellation = {});
}  // namespace Horo::Prefab
