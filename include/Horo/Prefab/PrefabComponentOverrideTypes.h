#pragma once
/** @file PrefabComponentOverrideTypes.h
 * @brief Owned component topology intent and preserved source-revision evidence for prefab instance transactions.
 */

#include "Horo/Prefab/PrefabIdentity.h"
#include "Horo/Prefab/PrefabSourceRevision.h"

#include <variant>
#include <vector>

namespace Horo::Prefab {
    /** @brief Exact component occurrence inside one stable prefab-local object scope. */
    struct PrefabComponentOverrideTarget final {
        PrefabObjectAddress object;
        Gameplay::ComponentTypeId type;
        PrefabComponentInstanceId instance;
        [[nodiscard]] auto operator<=>(const PrefabComponentOverrideTarget &) const noexcept = default;
    };

    /** @brief Add a fresh component identity with a complete payload pinned to its declared schema. */
    struct PrefabAddComponentOverride final {
        PrefabObjectAddress object;
        RawComponentPayload payload;
        [[nodiscard]] bool operator==(const PrefabAddComponentOverride &) const noexcept = default;
    };

    /** @brief Remove the exact source component only while its schema and canonical content still match. */
    struct PrefabRemoveComponentOverride final {
        PrefabComponentOverrideTarget target;
        std::uint32_t componentSchemaVersion{};
        Sha256Digest expectedSourceComponent;
        [[nodiscard]] bool operator==(const PrefabRemoveComponentOverride &) const noexcept = default;
    };

    /** @brief Closed ADR-093 topology algebra; replacement prepares a remove and a fresh add in one transaction. */
    using PrefabComponentOverrideRecord = std::variant<PrefabAddComponentOverride, PrefabRemoveComponentOverride>;

    /** @brief Missing or incompatible authority that prevents applying retained topology intent. */
    enum class PrefabComponentOverrideIssue : std::uint8_t {
        TargetRemoved,
        SchemaUnavailable,
        SchemaChanged
    };

    /** @brief Losslessly preserved component intent and the exact changed source evidence requiring explicit resolution. */
    struct PrefabComponentOverrideConflict final {
        PrefabComponentOverrideRecord original;
        PrefabSourceRevision authoredAgainst;
        Sha256Digest schemaRevision;
        Sha256Digest actualSourceComponent;
        PrefabSourceRevision observedSource;
        [[nodiscard]] bool operator==(const PrefabComponentOverrideConflict &) const noexcept = default;
    };

    /** @brief Owned original component intent retained across source deletion and unavailable or changed schema snapshots. */
    struct PrefabComponentOverrideOrphan final {
        PrefabComponentOverrideRecord original;
        PrefabSourceRevision authoredAgainst;
        Sha256Digest schemaRevision;
        PrefabComponentOverrideIssue reason{PrefabComponentOverrideIssue::TargetRemoved};
        [[nodiscard]] bool operator==(const PrefabComponentOverrideOrphan &) const noexcept = default;
    };

    /** @brief Remove intent whose exact source precondition is captured by the transaction preparer. */
    struct PrefabRemoveComponentEdit final {
        PrefabComponentOverrideTarget target;
    };

    /** @brief Atomic replacement with a fresh occurrence identity; persisted intent remains one remove plus one add. */
    struct PrefabReplaceComponentEdit final {
        PrefabComponentOverrideTarget target;
        RawComponentPayload replacement;
    };

    /** @brief Explicit selected-record revert; replacement reverts select both its old and new occurrence targets. */
    struct PrefabRevertComponentOverridesEdit final {
        std::vector<PrefabComponentOverrideTarget> targets;
    };

    /** @brief Closed authoring commands prepared without directly mutating a source or instance. */
    using PrefabComponentEdit =
        std::variant<PrefabAddComponentOverride, PrefabRemoveComponentEdit, PrefabReplaceComponentEdit, PrefabRevertComponentOverridesEdit>;
}  // namespace Horo::Prefab
