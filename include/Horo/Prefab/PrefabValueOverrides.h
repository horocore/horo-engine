#pragma once
/** @file PrefabValueOverrides.h
 * @brief Bounded schema capture, canonical value deltas and detached instance resolution.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Gameplay/BehaviorTypes.h"
#include "Horo/Prefab/PrefabSourceResolver.h"
#include "Horo/Prefab/PrefabValueOverrideTypes.h"

namespace Horo::Prefab {
    /** @brief Copied declarative schema for one stable registered project component property. No callbacks or module pointers. */
    struct PrefabValuePropertySchema final {
        Gameplay::ComponentTypeId componentType;
        PrefabPropertyId property;
        Gameplay::ComponentPropertyId payloadProperty; /**< Stable serialized property ID, never a display label. */
        std::uint32_t componentSchemaVersion{1};
        Gameplay::ComponentPropertyKind kind{Gameplay::ComponentPropertyKind::String};
        PrefabOverrideValue defaultValue;
        std::optional<double> minimum;
        std::optional<double> maximum;
        std::size_t maximumStringBytes{4096};
        bool required{};
    };

    /** @brief Captured prefab-only topology admission for one registered component schema; never executes provider code. */
    struct PrefabComponentOverridePolicy final {
        Gameplay::ComponentDescriptor component;
        std::uint32_t maximumInstances{1};
        bool allowAddition{};
        bool allowRemoval{};
        std::vector<Gameplay::ComponentTypeId> requiredComponents;
        std::vector<Gameplay::ComponentTypeId> incompatibleComponents;
        std::vector<Gameplay::BehaviorTypeId> incompatibleBehaviors;
    };

    /** @brief Explicit behavior/component compatibility captured before topology preparation; absence never implies permission. */
    struct PrefabBehaviorComponentPolicy final {
        Gameplay::BehaviorTypeId type;
        std::uint32_t schemaVersion{1};
        std::vector<Gameplay::ComponentTypeId> requiredComponents;
    };

    /** @brief Immutable owned schema metadata pinned across document preparation, package reload and worker resolution. */
    class PrefabValueSchemaSnapshot final {
    public:
        /** @brief Validate and copy stable schema metadata without invoking providers.
         * @param properties Complete explicitly composed property descriptors. @param limits Captured resource policy.
         * @param components Explicit component topology permissions and complete payload schemas.
         * @param behaviors Explicit compatibility contracts for behaviors on edited objects.
         * @return Owned snapshot or typed duplicate/schema/value/budget error. */
        [[nodiscard]] static Result<PrefabValueSchemaSnapshot> Create(std::vector<PrefabValuePropertySchema> properties,
                                                                      const PrefabLimitProfile &limits,
                                                                      std::vector<PrefabComponentOverridePolicy> components = {},
                                                                      std::vector<PrefabBehaviorComponentPolicy> behaviors = {});
        /** @brief Read immutable descriptors. @return Borrowed storage owned by this snapshot. */
        [[nodiscard]] std::span<const PrefabValuePropertySchema> Properties() const noexcept;
        /** @brief Read exact canonical schema identity. @return Stable digest of all validating metadata. */
        [[nodiscard]] Sha256Digest Revision() const noexcept;
        /** @brief Find one exact stable property. @param target Full component/property address.
         * @return Borrowed descriptor or null; no name/offset fallback. */
        [[nodiscard]] const PrefabValuePropertySchema *Find(const PrefabPropertyAddress &target) const noexcept;
        /** @brief Read captured component topology metadata. @return Borrowed immutable metadata owned by this snapshot. */
        [[nodiscard]] std::span<const PrefabComponentOverridePolicy> Components() const noexcept;
        /** @brief Read captured behavior compatibility metadata. @return Borrowed immutable metadata owned by this snapshot. */
        [[nodiscard]] std::span<const PrefabBehaviorComponentPolicy> Behaviors() const noexcept;
        /** @brief Find one exact component contract. @param type Stable registered type. @return Owned borrowed contract or null. */
        [[nodiscard]] const PrefabComponentOverridePolicy *FindComponent(const Gameplay::ComponentTypeId &type) const noexcept;
        /** @brief Find one exact behavior contract. @param type Stable registered type. @return Owned borrowed contract or null. */
        [[nodiscard]] const PrefabBehaviorComponentPolicy *FindBehavior(const Gameplay::BehaviorTypeId &type) const noexcept;

    private:
        /** @brief Publish completely validated owned inert metadata. */
        PrefabValueSchemaSnapshot(std::vector<PrefabValuePropertySchema> properties, const Sha256Digest &revision,
                                  std::vector<PrefabComponentOverridePolicy> components,
                                  std::vector<PrefabBehaviorComponentPolicy> behaviors) noexcept;
        std::vector<PrefabValuePropertySchema> properties_;
        Sha256Digest revision_;
        std::vector<PrefabComponentOverridePolicy> components_;
        std::vector<PrefabBehaviorComponentPolicy> behaviors_;
    };

    /** @brief Complete detached resolution; unresolved sets retain their intent but have no runtime candidate. */
    struct PrefabValueOverrideResolution final {
        std::optional<EffectivePrefabCandidate> effective;
        PrefabValueOverrideSet rebased;
        std::vector<PrefabValueProvenance> provenance;
        std::size_t redundantRecords{};
    };
    /** @brief Typed transactional authoring intent; revert removes a record rather than persisting an operation. */
    enum class PrefabValueEditKind : std::uint8_t {
        Assign,
        Revert,
        Rebase
    };

    /** @brief One owner-committed instance value edit, independent from any UI/widget. */
    struct PrefabValueEdit final {
        PrefabValueEditKind kind{PrefabValueEditKind::Assign};
        PrefabValueOverrideTarget target;
        PrefabOverrideValue value;
    };

    /** @brief Validate canonical storage and exact count/byte budgets without requiring live schemas.
     * @param set Owned authoring intent. @param limits Captured resource policy.
     * @return Success or typed invalid/conflicting/budget error; no mutation. */
    [[nodiscard]] Result<void> ValidatePrefabValueOverrideSet(const PrefabValueOverrideSet &set, const PrefabLimitProfile &limits);
    /** @brief Encode exact canonical bounded authoring data, including unresolved records.
     * @param set Valid or preserved authoring intent. @param limits Captured resource policy.
     * @return Canonical JSON bytes or typed validation/allocation error. */
    [[nodiscard]] Result<std::string> SerializePrefabValueOverrides(const PrefabValueOverrideSet &set, const PrefabLimitProfile &limits);
    /** @brief Decode bounded canonical value intent without discarding conflicts/orphans.
     * @param bytes Canonical JSON source. @param limits Captured resource policy.
     * @return Owned set or typed malformed/version/budget error. */
    [[nodiscard]] Result<PrefabValueOverrideSet> ParsePrefabValueOverrides(std::string_view bytes, const PrefabLimitProfile &limits);
    /** @brief Resolve against actual immutable prefab objects; source data is never mutated.
     * @param source Real canonical source expansion. @param set Current instance intent.
     * @param schemas Explicit immutable schema snapshot; null admits only transforms and preserves property records as orphans.
     * @param limits Captured resource policy. @param cancellation Cooperative operation token.
     * @return Detached effective candidate/provenance or preserved conflicts/orphans; malformed inputs fail without publication. */
    [[nodiscard]] Result<PrefabValueOverrideResolution> ResolvePrefabValueOverrides(const EffectivePrefabCandidate &source,
                                                                                    const PrefabValueOverrideSet &set,
                                                                                    const PrefabValueSchemaSnapshot *schemas,
                                                                                    const PrefabLimitProfile &limits,
                                                                                    const CancellationToken &cancellation = {});
    /** @brief Prepare one semantic assignment/revert/rebase through the same resolver as runtime conversion.
     * @param source Real revision-pinned source. @param current Optional existing owned intent. @param edit Typed operation.
     * @param schemas Explicit schema snapshot. @param limits Captured policy. @param cancellation Cooperative token.
     * @return Complete transaction candidate; caller validates document revision then atomically records history. */
    [[nodiscard]] Result<PrefabValueOverrideResolution> PreparePrefabValueEdit(
        const EffectivePrefabCandidate &source, const std::optional<PrefabValueOverrideSet> &current, const PrefabValueEdit &edit,
        const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits, const CancellationToken &cancellation = {});

    /** @brief Prepare complete component topology intent through the shared detached resolver and existing document history.
     * @param source Exact immutable immediate source. @param current Existing combined instance intent, if present.
     * @param edit Typed add/remove/replace/revert intent. @param schemas Captured complete validating metadata.
     * @param limits Captured resource policy. @param cancellation Cooperative operation token.
     * @return Complete transaction candidate or preserved conflict/orphan evidence; failure publishes no partial changes. */
    [[nodiscard]] Result<PrefabValueOverrideResolution> PreparePrefabComponentEdit(
        const EffectivePrefabCandidate &source, const std::optional<PrefabValueOverrideSet> &current, const PrefabComponentEdit &edit,
        const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits, const CancellationToken &cancellation = {});
}  // namespace Horo::Prefab
