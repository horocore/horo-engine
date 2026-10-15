#pragma once
/** @file PrefabValueOverrideTypes.h
 * @brief Owned, versioned instance value deltas and lossless conflict/provenance data.
 */
#include "Horo/Math/SceneMath.h"
#include "Horo/Prefab/PrefabIdentity.h"
#include "Horo/Prefab/PrefabSourceRevision.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Horo::Prefab {
    /** @brief Closed schema-typed value carrier; transforms are whole-object local TRS assignments. */
    using PrefabOverrideValue =
        std::variant<bool, std::int64_t, double, std::string, Math::Vec2, Math::Vec3, Math::Quaternion, Math::Transform>;
    /** @brief Exact prefab-local transform or registered component/property target. */
    using PrefabValueOverrideTarget = std::variant<PrefabObjectAddress, PrefabPropertyAddress>;

    /** @brief One AssignValue operation with an exact immediate-source precondition. */
    struct PrefabValueOverrideRecord final {
        PrefabValueOverrideTarget target;
        std::uint32_t componentSchemaVersion{}; /**< Zero only for the built-in object transform. */
        Sha256Digest expectedSourceValue;
        PrefabOverrideValue value;
        [[nodiscard]] bool operator==(const PrefabValueOverrideRecord &) const noexcept = default;
    };
    /** @brief Source or schema change that requires explicit authoring resolution. */
    enum class PrefabValueOverrideIssue : std::uint8_t {
        SourceChanged,
        TargetRemoved,
        SchemaUnavailable,
        SchemaChanged
    };

    /** @brief Preserved original intent and exact new-source evidence; never silently resolved. */
    struct PrefabValueOverrideConflict final {
        PrefabValueOverrideRecord original;
        PrefabSourceRevision authoredAgainst;
        Sha256Digest schemaRevision;
        Sha256Digest actualSourceValue;
        PrefabSourceRevision observedSource;
        [[nodiscard]] bool operator==(const PrefabValueOverrideConflict &) const noexcept = default;
    };

    /** @brief Original record retained when its target/schema cannot be used. */
    struct PrefabValueOverrideOrphan final {
        PrefabValueOverrideRecord original;
        PrefabSourceRevision authoredAgainst;
        Sha256Digest schemaRevision;
        PrefabValueOverrideIssue reason{PrefabValueOverrideIssue::TargetRemoved};
        [[nodiscard]] bool operator==(const PrefabValueOverrideOrphan &) const noexcept = default;
    };

    /** @brief Canonical instance-local AssignValue set; unresolved intent remains serializable but cannot enter runtime output. */
    struct PrefabValueOverrideSet final {
        Assets::AssetId sourcePrefab;
        PrefabSourceRevision authoredAgainst;
        Sha256Digest schemaRevision; /**< Exact immutable schema digest; zero for transform-only sets. */
        std::vector<PrefabValueOverrideRecord> records;
        std::vector<PrefabValueOverrideConflict> conflicts;
        std::vector<PrefabValueOverrideOrphan> orphans;
        [[nodiscard]] bool operator==(const PrefabValueOverrideSet &) const noexcept = default;
    };
    /** @brief Authority supplying the effective value; display text never determines provenance. */
    enum class PrefabValueOrigin : std::uint8_t {
        Source,
        Inherited,
        Instance
    };

    /** @brief Value-only editor evidence retaining registry, immediate-source and effective values separately. */
    struct PrefabValueProvenance final {
        PrefabValueOverrideTarget target;
        PrefabValueOrigin origin{PrefabValueOrigin::Source};
        std::optional<PrefabOverrideValue>
            schemaDefault; /**< Registry creation default for inspection only; never substitutes an absent source property. */
        PrefabOverrideValue sourceValue;
        PrefabOverrideValue effectiveValue;
        Assets::AssetId sourcePrefab;
        [[nodiscard]] bool operator==(const PrefabValueProvenance &) const noexcept = default;
    };
}  // namespace Horo::Prefab
