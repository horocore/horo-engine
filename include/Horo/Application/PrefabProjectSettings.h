#pragma once
/** @file PrefabProjectSettings.h
 * @brief Typed project prefab configuration projected through the canonical Foundation resolver.
 */
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Foundation/Configuration.h"
#include "Horo/Prefab/PrefabLimits.h"

#include <array>
#include <memory>

namespace Horo::Application {
    /** @brief Closed editable project-policy fields; derived work and parser safety are deliberately excluded. */
    enum class PrefabLimitField : std::uint8_t {
        HierarchyDepth,
        ObjectCount,
        SourcePayloadBytes,
        ExpandedPayloadBytes,
        CookedPayloadBytes,
        ComponentsPerObject,
        ReferencedAssets,
        DirectNestedPlacements,
        VariantInheritanceDepth,
        NestedPrefabDepth,
        OverrideRecords,
        ConflictAndOrphanRecords,
        PropertyPathSegments,
        OverrideValueBytes,
        OverrideSetBytes,
        BindingSlots,
        BindingUses,
        InstanceBindings,
        RuntimeSpawnDepth,
        Count
    };
    inline constexpr std::size_t PrefabLimitFieldCount = static_cast<std::size_t>(PrefabLimitField::Count);

    /** @brief Canonical presentation/configuration metadata for one typed field, shared by every host. */
    struct PrefabLimitFieldDescriptor final {
        PrefabLimitField field;
        std::string_view key;
        std::string_view localizationKey;
        std::size_t hardCeiling;
        std::size_t defaultValue;
    };

    /** @brief Signed draft values preserve invalid negative input until explicit correction/reset. */
    using PrefabPolicyValues = std::array<std::int64_t, PrefabLimitFieldCount>;
    /** @brief Stable localized validation reason; no UI string determines admission. */
    enum class PrefabPolicyFinding : std::uint8_t {
        None,
        InvalidNumber,
        OutsideSafetyCeiling,
        ValueExceedsSet
    };

    /** @brief Complete immutable-by-value field findings and canonical accepted profile, if valid. */
    struct PrefabPolicyValidation final {
        std::array<PrefabPolicyFinding, PrefabLimitFieldCount> fields{};
        std::optional<Prefab::PrefabLimitProfile> profile;
    };

    /** @brief Reads the one deterministic editable field registry. @return Process-lifetime immutable metadata. */
    [[nodiscard]] std::span<const PrefabLimitFieldDescriptor> PrefabLimitFields() noexcept;
    /** @brief Projects canonical typed policy values. @param policy Project profile value. @return Ordered signed values. */
    [[nodiscard]] PrefabPolicyValues PrefabPolicyFieldValues(const Prefab::PrefabProjectPolicy &policy) noexcept;
    /** @brief Validates all fields and canonical cross-field/work arithmetic. @param values Complete typed draft.
     * @return Field findings and a profile only when the entire candidate is valid. */
    [[nodiscard]] PrefabPolicyValidation ValidatePrefabPolicyValues(const PrefabPolicyValues &values);
    /** @brief Creates the project-only NextOperation schema using the canonical field registry.
     * @return Sealed schema, or original descriptor error. */
    [[nodiscard]] Result<ConfigurationSchema> CreatePrefabProjectSettingsSchema();

    /** @brief Immutable resolved project configuration; no runtime or document owner is retained. */
    class PrefabProjectSettings final {
    public:
        /** @brief Resolve one complete project map through Foundation and validate its domain projection.
         * @param project Captured project source; absence selects declared engine defaults.
         * @param revision Host-issued configuration publication identity.
         * @return Complete immutable publication or typed configuration/policy failure. */
        [[nodiscard]] static Result<PrefabProjectSettings> Resolve(const ConfigurationSourceMap &project,
                                                                   ConfigurationRevision revision = 1);
        /** @brief Resolve an explicit typed candidate through the same project-source authority.
         * @param values Complete typed candidate. @param revision Host-issued publication.
         * @return Immutable accepted candidate or typed field/domain failure. */
        [[nodiscard]] static Result<PrefabProjectSettings> Create(const PrefabPolicyValues &values, ConfigurationRevision revision = 1);

        /** @brief Returns immutable resolved configuration and provenance. @return Owned snapshot handle. */
        [[nodiscard]] const ConfigurationSnapshot &Configuration() const noexcept {
            return configuration_;
        }

        /** @brief Returns canonical next-operation profile. @return Borrowed immutable policy. */
        [[nodiscard]] const Prefab::PrefabLimitProfile &Limits() const noexcept {
            return limits_;
        }

    private:
        PrefabProjectSettings(ConfigurationSnapshot configuration, Prefab::PrefabLimitProfile limits)
            : configuration_(std::move(configuration)), limits_(std::move(limits)) {}

        ConfigurationSnapshot configuration_;
        Prefab::PrefabLimitProfile limits_;
    };

    /** @brief Exact project-file identity paired with one immutable configuration. Missing files explicitly select defaults. */
    struct PrefabProjectSettingsFile final {
        std::shared_ptr<const PrefabProjectSettings> settings;
        Assets::AssetSourceIdentity identity;
    };

    /** @brief A completed atomic publication; post-rename durability uncertainty remains explicit. */
    struct PrefabProjectSettingsPublication final {
        PrefabProjectSettingsFile file;
        std::optional<Error> durabilityError;
    };

    /** @brief Capture the optional canonical module project document with physical containment and bounded reads.
     * @param projectRoot Authorized canonical absolute root. @param revision Host publication revision.
     * @return Accepted immutable settings or original parse/identity/policy failure. */
    [[nodiscard]] Result<PrefabProjectSettingsFile> LoadPrefabProjectSettings(const std::filesystem::path &projectRoot,
                                                                              ConfigurationRevision revision = 1);
    /** @brief Atomically publish a validated candidate with an exact source-identity fence and native writer lock.
     * @param projectRoot Authorized root. @param expected Exact prior capture, including absence.
     * @param candidate Complete immutable successor. @param files Host durable filesystem.
     * @return Actual publication receipt; caller owns project mutation admission and next-operation activation. */
    [[nodiscard]] Result<PrefabProjectSettingsPublication> SavePrefabProjectSettings(const std::filesystem::path &projectRoot,
                                                                                     const PrefabProjectSettingsFile &expected,
                                                                                     const PrefabProjectSettings &candidate,
                                                                                     DurableFileSystem &files);
}  // namespace Horo::Application
