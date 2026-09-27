#include "Horo/Runtime/Save/SaveArchiveMetadata.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace Horo::Runtime {
    namespace {
        enum class VersionAdmission : std::uint8_t {
            Direct,
            Migration,
            Rejected,
        };

        /** @brief Classifies one independent version without conflating its schema authority. */
        template <typename Tag>
        [[nodiscard]] VersionAdmission ClassifyVersion(const SaveVersion<Tag> version, const SaveVersionSupport<Tag> &support) noexcept {
            using enum VersionAdmission;
            if (support.direct.Contains(version))
                return Direct;
            if (support.migrationSource && support.migrationSource->Contains(version))
                return Migration;
            return Rejected;
        }

        /** @brief Reports whether one version support declaration has valid, non-overlapping ranges. */
        template <typename Tag> [[nodiscard]] bool IsValidSupport(const SaveVersionSupport<Tag> &support) noexcept {
            if (!support.direct.minimum.IsValid() || !support.direct.maximum.IsValid() || support.direct.minimum > support.direct.maximum)
                return false;
            if (!support.migrationSource)
                return true;
            const auto &migration = *support.migrationSource;
            const bool migrationRangeValid =
                migration.minimum.IsValid() && migration.maximum.IsValid() && migration.minimum <= migration.maximum;
            const bool rangesDisjoint = migration.maximum < support.direct.minimum || support.direct.maximum < migration.minimum;
            return migrationRangeValid && rangesDisjoint;
        }

        /** @brief Creates a rejected compatibility decision. */
        [[nodiscard]] SaveCompatibilityDecision Reject(const SaveCompatibilityReason reason,
                                                       std::optional<SaveParticipantId> participant = std::nullopt) {
            return {.disposition = SaveCompatibilityDisposition::Rejected, .reason = reason, .participant = std::move(participant)};
        }

        /** @brief Applies one root compatibility axis and records whether migration is required. */
        [[nodiscard]] std::optional<SaveCompatibilityDecision> EvaluateRootVersion(const VersionAdmission admission,
                                                                                   const SaveCompatibilityReason reason,
                                                                                   bool &migrationRequired) {
            if (admission == VersionAdmission::Rejected)
                return Reject(reason);
            migrationRequired |= admission == VersionAdmission::Migration;
            return std::nullopt;
        }

        /** @brief Evaluates the three independent root version axes in their normative order. */
        [[nodiscard]] std::optional<SaveCompatibilityDecision> EvaluateRootVersions(const ArchiveFormatVersion archiveVersion,
                                                                                    const SaveArchiveHeader &header,
                                                                                    const SaveGameManifest &manifest,
                                                                                    const SaveCompatibilityPolicy &policy,
                                                                                    bool &migrationRequired) {
            using enum SaveCompatibilityReason;
            if (auto rejected = EvaluateRootVersion(ClassifyVersion(archiveVersion, policy.archiveVersions), UnsupportedArchiveVersion,
                                                    migrationRequired))
                return rejected;
            if (auto rejected = EvaluateRootVersion(ClassifyVersion(manifest.saveSchemaVersion, policy.saveSchemaVersions),
                                                    UnsupportedSaveSchema, migrationRequired))
                return rejected;
            return EvaluateRootVersion(ClassifyVersion(header.productCompatibility, policy.productVersions), UnsupportedProductVersion,
                                       migrationRequired);
        }

        /** @brief Validates stable participant policy order and independent version ranges. */
        [[nodiscard]] bool HasValidParticipantPolicy(const SaveCompatibilityPolicy &policy) noexcept {
            if (!std::ranges::is_sorted(policy.participants, {}, &SaveParticipantCompatibility::participant))
                return false;
            for (std::size_t index = 0; index < policy.participants.size(); ++index) {
                const auto &support = policy.participants[index];
                if (!support.participant.IsValid() || !IsValidSupport(support.versions) ||
                    (index != 0 && policy.participants[index - 1].participant == support.participant) ||
                    !std::ranges::is_sorted(support.requiredDependencies) ||
                    std::ranges::adjacent_find(support.requiredDependencies) != support.requiredDependencies.end() ||
                    std::ranges::any_of(support.requiredDependencies, [&support](const SaveParticipantId &id) {
                    return !id.IsValid() || id == support.participant;
                }))
                    return false;
            }
            if (!std::ranges::is_sorted(policy.droppableUnknownParticipants) ||
                std::ranges::adjacent_find(policy.droppableUnknownParticipants) != policy.droppableUnknownParticipants.end())
                return false;
            for (const SaveParticipantId &id : policy.droppableUnknownParticipants) {
                if (!id.IsValid() || std::ranges::binary_search(policy.participants, id, {}, &SaveParticipantCompatibility::participant) ||
                    std::ranges::any_of(policy.participants, [&id](const SaveParticipantCompatibility &support) {
                    return support.required && std::ranges::binary_search(support.requiredDependencies, id);
                }))
                    return false;
            }
            return true;
        }

        /** @brief Validates metadata and root policy ranges before compatibility classification. */
        [[nodiscard]] bool HasValidCompatibilityInputs(const SaveArchiveHeader &header, const SaveGameManifest &manifest,
                                                       const SaveCompatibilityPolicy &policy, const SaveArchiveMetadataLimits &limits) {
            return ValidateSaveArchiveHeader(header, limits).HasValue() && ValidateSaveGameManifest(manifest, limits).HasValue() &&
                   IsValidSupport(policy.archiveVersions) && IsValidSupport(policy.saveSchemaVersions) &&
                   IsValidSupport(policy.productVersions) && HasValidParticipantPolicy(policy);
        }

        /** @brief Finds the first required dependency missing from a manifest's sorted participant list. */
        [[nodiscard]] std::optional<SaveParticipantId> MissingRequiredDependency(const SaveGameManifest &manifest,
                                                                                 const SaveParticipantCompatibility &support) {
            for (const SaveParticipantId &dependency : support.requiredDependencies) {
                if (!std::ranges::binary_search(manifest.participants, dependency, {}, &SaveManifestParticipant::participant))
                    return dependency;
            }
            return std::nullopt;
        }

        /** @brief Evaluates declared participant support and required current composition. */
        [[nodiscard]] std::optional<SaveCompatibilityDecision> EvaluateDeclaredParticipants(const SaveGameManifest &manifest,
                                                                                            const SaveCompatibilityPolicy &policy,
                                                                                            bool &migrationRequired) {
            using enum SaveCompatibilityReason;
            using enum VersionAdmission;
            for (const auto &support : policy.participants) {
                const auto found =
                    std::ranges::lower_bound(manifest.participants, support.participant, {}, &SaveManifestParticipant::participant);
                if (found == manifest.participants.end() || found->participant != support.participant) {
                    if (support.required)
                        return Reject(MissingRequiredParticipant, support.participant);
                    continue;
                }
                if (support.required)
                    if (const auto missing = MissingRequiredDependency(manifest, support))
                        return Reject(MissingRequiredParticipant, *missing);
                const VersionAdmission admission = ClassifyVersion(found->schemaVersion, support.versions);
                if (admission == Rejected && (support.required || found->required))
                    return Reject(UnsupportedParticipantSchema, support.participant);
                if (admission == Rejected)
                    continue;
                migrationRequired |= admission == Migration;
            }
            return std::nullopt;
        }

        /** @brief Rejects required manifest participants absent from the sealed policy. */
        [[nodiscard]] std::optional<SaveCompatibilityDecision> FindUnknownRequiredParticipant(const SaveGameManifest &manifest,
                                                                                              const SaveCompatibilityPolicy &policy) {
            for (const SaveManifestParticipant &entry : manifest.participants) {
                const auto found =
                    std::ranges::lower_bound(policy.participants, entry.participant, {}, &SaveParticipantCompatibility::participant);
                if (entry.required && (found == policy.participants.end() || found->participant != entry.participant))
                    return Reject(SaveCompatibilityReason::UnknownRequiredParticipant, entry.participant);
                for (const SaveParticipantCompatibility &support : policy.participants) {
                    if (!support.required || !std::ranges::binary_search(support.requiredDependencies, entry.participant))
                        continue;
                    if (found == policy.participants.end() || found->participant != entry.participant ||
                        ClassifyVersion(entry.schemaVersion, found->versions) == VersionAdmission::Rejected)
                        return Reject(SaveCompatibilityReason::UnknownRequiredParticipant, entry.participant);
                }
            }
            return std::nullopt;
        }
    }  // namespace

    /** @copydoc EvaluateSaveCompatibility */
    SaveCompatibilityDecision EvaluateSaveCompatibility(const ArchiveFormatVersion archiveVersion, const SaveArchiveHeader &header,
                                                        const SaveGameManifest &manifest, const SaveCompatibilityPolicy &policy) {
        using enum SaveCompatibilityReason;
        SaveArchiveMetadataLimits limits;
        limits.supportedFeatureFlagsMask = std::numeric_limits<std::uint64_t>::max();
        if (!HasValidCompatibilityInputs(header, manifest, policy, limits))
            return Reject(InvalidMetadata);

        bool migrationRequired = false;
        if (auto rejected = EvaluateRootVersions(archiveVersion, header, manifest, policy, migrationRequired))
            return *rejected;
        if ((header.featureFlags & ~policy.supportedFeatureFlagsMask) != 0)
            return Reject(UnsupportedFeature);
        if (auto rejected = EvaluateDeclaredParticipants(manifest, policy, migrationRequired))
            return *rejected;
        const SaveCompatibilityDecision compatible{.disposition = migrationRequired ? SaveCompatibilityDisposition::MigrationRequired
                                                                                    : SaveCompatibilityDisposition::DirectRead,
                                                   .reason = None,
                                                   .participant = std::nullopt};
        return FindUnknownRequiredParticipant(manifest, policy).value_or(compatible);
    }
}  // namespace Horo::Runtime
