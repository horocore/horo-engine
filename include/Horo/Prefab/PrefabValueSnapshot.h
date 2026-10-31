#pragma once
/** @file PrefabValueSnapshot.h @brief Bounded immutable value diffs and exact preview revision proofs. */
#include "Horo/Prefab/PrefabValueOverrides.h"

#include <memory>

namespace Horo::Prefab {
    /** @brief Exact source, schema and authored delta capture; every field participates in freshness. */
    struct PrefabValueSnapshotRevision final {
        PrefabResolutionRevision source;
        Sha256Digest schema;
        Sha256Digest delta; /**< Zero denotes absent instance intent, distinct from a present empty envelope. */
        [[nodiscard]] bool operator==(const PrefabValueSnapshotRevision &) const noexcept = default;
    };

    /** @brief Explicit presentation ceilings; canonical resolution retains its separate project limits. */
    struct PrefabValueSnapshotLimits final {
        std::size_t maximumEntries{256};
        std::size_t maximumBytes{1024 * 1024};
    };

    /** @brief Complete semantic cardinality and retained prefix; omission never implies absence. */
    struct PrefabValueSnapshotCoverage final {
        std::size_t total{};
        std::size_t retained{};

        [[nodiscard]] bool Complete() const noexcept {
            return total == retained;
        }
    };

    /** @brief Exact opaque component evidence; unknown properties are never decoded or synthesized. */
    struct PrefabOpaqueValueEvidence final {
        ExpandedPrefabObjectKey object;
        Gameplay::ComponentTypeId type;
        PrefabComponentInstanceId instance;
        std::uint32_t schemaVersion{};
        Sha256Digest payload;
        std::size_t payloadBytes{};
    };

    struct PrefabValueSnapshotPreparation;
    /** @brief Exact canonical applied, conflicting or orphaned component intent; no opaque payload is interpreted as properties. */
    using PrefabComponentDiffEvidence =
        std::variant<PrefabComponentOverrideRecord, PrefabComponentOverrideConflict, PrefabComponentOverrideOrphan>;

    /** @brief Owned read-only values and preserved intent, independent of document/provider lifetime. */
    class PrefabValueSnapshot final {
    public:
        /** @brief Resolves through the canonical value engine exactly once and captures bounded inspection.
         * @param source Immutable unmodified expansion. @param authored Optional exact scene-instance intent.
         * @param schemas Immutable registered properties, or null for transform/opaque inspection.
         * @param limits Canonical operation policy. @param capture Presentation row/byte ceilings.
         * @param cancellation Cooperative stop. @return Canonical resolution plus snapshot or explicit inspection unavailability;
         * canonical admission failures remain errors. Inspection failure never changes a successful canonical resolution.
         */
        [[nodiscard]] static Result<PrefabValueSnapshotPreparation> Prepare(const EffectivePrefabCandidate &source,
                                                                            const std::optional<PrefabValueOverrideSet> &authored,
                                                                            const PrefabValueSchemaSnapshot *schemas,
                                                                            const PrefabLimitProfile &limits,
                                                                            PrefabValueSnapshotLimits capture = {},
                                                                            const CancellationToken &cancellation = {});

        /** @brief Read captured revision proof. @return Owned immutable capture. */
        [[nodiscard]] const PrefabValueSnapshotRevision &Revision() const noexcept {
            return revision_;
        }

        /** @brief Compare all captured authorities. @param current Exact new capture. @return Whether reuse is permitted. */
        [[nodiscard]] bool IsCurrent(const PrefabValueSnapshotRevision &current) const noexcept {
            return revision_ == current;
        }

        /** @brief Read typed source/effective/default values and actual origin chains. @return Deterministic retained prefix. */
        [[nodiscard]] std::span<const PrefabValueProvenance> Values() const noexcept {
            return values_;
        }

        /** @brief Read preserved conflicting intent. @return Deterministic retained prefix. */
        [[nodiscard]] std::span<const PrefabValueOverrideConflict> Conflicts() const noexcept {
            return conflicts_;
        }

        /** @brief Read preserved orphan intent. @return Deterministic retained prefix. */
        [[nodiscard]] std::span<const PrefabValueOverrideOrphan> Orphans() const noexcept {
            return orphans_;
        }

        /** @brief Read unavailable typed-property evidence. @return Exact opaque component identities and payload commitments. */
        [[nodiscard]] std::span<const PrefabOpaqueValueEvidence> Opaque() const noexcept {
            return opaque_;
        }

        /** @brief Read current supported component add/remove inspection. @return Canonical deterministic applied/conflict/orphan prefixes.
         */
        [[nodiscard]] std::span<const PrefabComponentDiffEvidence> Components() const noexcept {
            return components_;
        }

        /** @brief Read aggregate value/conflict/orphan/opaque coverage. @return Complete cardinality and retained count. */
        [[nodiscard]] PrefabValueSnapshotCoverage Coverage() const noexcept {
            return coverage_;
        }

        /** @brief Read charged retained presentation storage. @return Bounded carrier and variable-payload byte charge. */
        [[nodiscard]] std::size_t RetainedBytes() const noexcept {
            return retainedBytes_;
        }

    private:
        PrefabValueSnapshot() = default;
        PrefabValueSnapshotRevision revision_;
        std::vector<PrefabValueProvenance> values_;
        std::vector<PrefabValueOverrideConflict> conflicts_;
        std::vector<PrefabValueOverrideOrphan> orphans_;
        std::vector<PrefabOpaqueValueEvidence> opaque_;
        std::vector<PrefabComponentDiffEvidence> components_;
        PrefabValueSnapshotCoverage coverage_;
        std::size_t retainedBytes_{};
    };

    /** @brief One canonical preparation: immutable inspection and authoritative resolver output share the same capture. */
    struct PrefabValueSnapshotPreparation final {
        std::shared_ptr<const PrefabValueSnapshot> snapshot;
        PrefabValueOverrideResolution resolution;
        std::optional<Error> inspectionFailure; /**< Optional observation failed after canonical resolution succeeded. */
    };

    /** @brief Capture exact preview cache evidence without resolving values or retaining mutable services.
     * @param source Current immutable expansion. @param authored Exact instance intent or absence.
     * @param schemas Current immutable schema. @param limits Canonical delta serialization policy.
     * @return Owned proof or typed invalid/budget/allocation failure.
     */
    [[nodiscard]] Result<PrefabValueSnapshotRevision> CapturePrefabValueSnapshotRevision(
        const EffectivePrefabCandidate &source, const std::optional<PrefabValueOverrideSet> &authored,
        const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits);
}  // namespace Horo::Prefab
