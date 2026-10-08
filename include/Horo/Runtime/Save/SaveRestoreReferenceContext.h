#pragma once

/** @file SaveRestoreReferenceContext.h
 * @brief Immutable schema-owned stable reference results delivered to inactive owner candidates.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveIdentity.h"

#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace Horo::Runtime {
    /** @brief Stable identity assigned to one reference field by its participant payload schema. */
    struct SaveRestoreReferenceId final {
        std::uint64_t value{};
        auto operator<=>(const SaveRestoreReferenceId &) const noexcept = default;
    };

    /** @brief Exact durable entity target verified against the unpublished aggregate identity map. */
    struct SaveRestoreEntityTarget final {
        PersistentEntityId identity;
        std::uint64_t incarnation{};
    };

    /** @brief Exact component owner; the payload retains its typed component type identity unchanged. */
    struct SaveRestoreComponentTarget final {
        SaveRestoreEntityTarget owner;
    };

    /** @brief Cooked logical asset target pinned by the unpublished Scene. */
    struct SaveRestoreAssetTarget final {
        SaveAssetId identity;
    };

    /** @brief Exact active project-service generation; the payload retains its typed service identity unchanged. */
    struct SaveRestoreServiceTarget final {
        std::uint64_t generation{};
    };

    /** @brief Exact pinned participant target and optional declared record. */
    struct SaveRestoreParticipantTarget final {
        SaveParticipantId identity;
        ParticipantSchemaVersion schema;
        std::optional<SaveRecordId> record;
    };

    /** @brief Resolved stable forms without runtime Scene types, pointers or native handles. */
    using SaveRestoreReferenceTarget = std::variant<SaveRestoreEntityTarget, SaveRestoreComponentTarget, SaveRestoreAssetTarget,
                                                    SaveRestoreServiceTarget, SaveRestoreParticipantTarget>;
    /** @brief Explicit reconciliation outcome; a missing required reference is never admitted. */
    enum class SaveRestoreReferenceDisposition : std::uint8_t {
        Resolved,
        Remapped,
        OptionalAbsent
    };

    /** @brief One owned result addressed by participant and schema-defined reference identity. */
    struct SaveRestoreReferenceResult final {
        SaveParticipantId owner;
        SaveRestoreReferenceId reference;
        SaveRestoreReferenceDisposition disposition{SaveRestoreReferenceDisposition::Resolved};
        bool required{true};
        SaveRestoreReferenceTarget target; /**< Resolved target, or original target for explicit optional absence. */
    };

    /** @brief Exact aggregate generations shared by every schema-owned result. */
    struct SaveRestoreReferenceGeneration final {
        std::uint64_t session{};
        std::uint64_t registry{};
        std::uint64_t candidateScene{};
    };

    /**
     * @brief Immutable participant-filtered view of an operation-owned reference context.
     * @details The aggregate owns this context and all target lifetime pins through completion/rollback.
     *          Owner callbacks may copy stable result values, never retain the borrowed view.
     */
    struct SaveRestoreReferenceView final {
        SaveRestoreReferenceGeneration generation;
        std::span<const SaveRestoreReferenceResult> references;
        /** @brief Finds one exact schema reference. @param reference Schema-owned identity.
         * @return Borrowed result or nullptr when it was not declared. */
        [[nodiscard]] const SaveRestoreReferenceResult *Find(SaveRestoreReferenceId reference) const noexcept;
    };

    /** @brief Bounded immutable reference results constructed only after identity allocation and state application. */
    class SaveRestoreReferenceContext final {
    public:
        SaveRestoreReferenceContext() = default;
        /** @brief Validates complete stable results before callbacks receive them.
         * @param generation Exact owning aggregate generations.
         * @param results Complete result set in arbitrary order.
         * @param maximumReferences Trusted positive bound at most 65,536.
         * @return Immutable context or typed invalid, duplicate, budget or allocation failure. */
        [[nodiscard]] static Result<SaveRestoreReferenceContext> Create(SaveRestoreReferenceGeneration generation,
                                                                        std::span<const SaveRestoreReferenceResult> results,
                                                                        std::size_t maximumReferences = 65'536);
        /** @brief Returns only one participant's declared references. @param owner Exact schema owner.
         * @return Borrowed immutable view; an empty view declares no references for this owner. */
        [[nodiscard]] SaveRestoreReferenceView ForParticipant(const SaveParticipantId &owner) const noexcept;
        /** @brief Returns exact aggregate evidence. @return Generation values, zero only for an empty default context. */
        [[nodiscard]] SaveRestoreReferenceGeneration Generation() const noexcept;
        /** @brief Returns every validated result for aggregate owner-admission checks. @return Immutable owned view. */
        [[nodiscard]] std::span<const SaveRestoreReferenceResult> Results() const noexcept;

    private:
        SaveRestoreReferenceContext(SaveRestoreReferenceGeneration generation, std::vector<SaveRestoreReferenceResult> results) noexcept;
        SaveRestoreReferenceGeneration generation_;
        std::vector<SaveRestoreReferenceResult> results_;
    };
}  // namespace Horo::Runtime
