#pragma once

/**
 * @file SaveArchiveMetadata.h
 * @brief Canonical bounded save header, manifest, and compatibility contracts.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Save/SaveIdentity.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Runtime {
    struct SaveProjectIdentityTag;
    struct SaveWorldIdentityTag;
    struct SaveBaseSceneIdentityTag;

    /** @brief Stable project identity recorded by save metadata. */
    using SaveProjectId = PersistentSaveIdentity<SaveProjectIdentityTag>;
    /** @brief Stable logical world identity recorded by save metadata. */
    using SaveWorldId = PersistentSaveIdentity<SaveWorldIdentityTag>;
    /** @brief Stable base-scene asset identity recorded by save metadata. */
    using SaveBaseSceneId = PersistentSaveIdentity<SaveBaseSceneIdentityTag>;

    /** @brief Typed logical-state digest, distinct from archive byte integrity. */
    struct CanonicalStateHash final {
        Sha256Digest value; /**< Domain-separated digest of canonical logical state. */
        [[nodiscard]] constexpr auto operator<=>(const CanonicalStateHash &) const noexcept = default;
    };

    /** @brief Typed immutable archive-content digest carried by the integrity trailer. */
    struct ArchiveContentHash final {
        Sha256Digest value; /**< Domain-separated digest of the finalized preamble and payload. */
        [[nodiscard]] constexpr auto operator<=>(const ArchiveContentHash &) const noexcept = default;
    };

    /** @brief Explicit bounds applied before or while decoding save metadata. */
    struct SaveArchiveMetadataLimits final {
        std::size_t maximumHeaderBytes{64U * 1024U};    /**< Maximum encoded header bytes. */
        std::size_t maximumManifestBytes{256U * 1024U}; /**< Maximum encoded manifest bytes. */
        std::size_t maximumTextBytes{256};              /**< Maximum bytes in one provenance string. */
        std::size_t maximumParticipants{256};           /**< Maximum manifest participant entries. */
        std::size_t maximumChunksPerParticipant{4'096}; /**< Maximum chunk references per participant. */
        std::size_t maximumTotalChunks{16'384};         /**< Aggregate manifest chunk reference limit. */
        std::size_t maximumNestingDepth{8};             /**< Maximum JSON object/array nesting depth. */
        std::uint64_t supportedFeatureFlagsMask{};      /**< Feature bits understood by this codec. */
    };

    /** @brief Canonical metadata available before gameplay payload decoding. */
    struct SaveArchiveHeader final {
        SaveGameSlotId slot;                                  /**< Logical slot identity. */
        SlotGenerationId slotGeneration;                      /**< Exact publication generation. */
        std::optional<SlotGenerationId> parentGeneration;     /**< Expected replaced generation, when any. */
        ProductSaveCompatibilityVersion productCompatibility; /**< Producing product policy version. */
        ProductStorageId product;                             /**< Product storage namespace identity. */
        EnvironmentStorageId environment;                     /**< Production/development/test partition. */
        LocalUserStorageId user;                              /**< Product-local user scope. */
        GameProfileId profile;                                /**< Product-owned game profile. */
        SaveProjectId project;                                /**< Stable project identity. */
        SaveWorldId world;                                    /**< Stable logical world identity. */
        SaveBaseSceneId baseScene;                            /**< Stable compatible base scene identity. */
        std::uint64_t capturedAtUnixMilliseconds{};           /**< Provenance timestamp, never causality. */
        std::uint64_t playTimeNanoseconds{};                  /**< Bounded accumulated semantic play time. */
        std::string engineVersion;                            /**< Bounded diagnostic engine version. */
        std::string projectBuildId;                           /**< Bounded diagnostic project build identity. */
        std::uint64_t featureFlags{};                         /**< Required metadata feature bits. */

        [[nodiscard]] auto operator<=>(const SaveArchiveHeader &) const noexcept = default;
    };

    /** @brief One participant and its stable chunk references in the save manifest. */
    struct SaveManifestParticipant final {
        SaveParticipantId participant;          /**< Canonical participant identity. */
        ParticipantSchemaVersion schemaVersion; /**< Independent participant schema. */
        bool required{true};                    /**< Whether absence or unknown support rejects restore. */
        std::vector<SaveRecordId> chunks;       /**< Non-empty, unique, stable-sorted chunk identities. */

        [[nodiscard]] auto operator<=>(const SaveManifestParticipant &) const noexcept = default;
    };

    /** @brief Canonical manifest that can be inspected without decoding participant payloads. */
    struct SaveGameManifest final {
        SaveSchemaVersion saveSchemaVersion;               /**< Whole-save canonical schema. */
        CanonicalStateHash canonicalState;                 /**< Logical-state equivalence identity. */
        std::vector<SaveManifestParticipant> participants; /**< Stable participant-identity order. */

        [[nodiscard]] auto operator<=>(const SaveGameManifest &) const noexcept = default;
    };

    /** @brief Inclusive support range on one independent save version axis. */
    template <typename Tag> struct SaveVersionRange final {
        SaveVersion<Tag> minimum; /**< Oldest supported value. */
        SaveVersion<Tag> maximum; /**< Newest supported value. */

        [[nodiscard]] constexpr bool Contains(const SaveVersion<Tag> version) const noexcept {
            return minimum.IsValid() && maximum.IsValid() && minimum <= maximum && version >= minimum && version <= maximum;
        }
    };

    /** @brief Direct-read and optional migration-source ranges for one version axis. */
    template <typename Tag> struct SaveVersionSupport final {
        SaveVersionRange<Tag> direct;                         /**< Directly readable versions. */
        std::optional<SaveVersionRange<Tag>> migrationSource; /**< Versions accepted only through migration. */
    };

    /** @brief Release support declaration for one participant schema. */
    struct SaveParticipantCompatibility final {
        SaveParticipantId participant;                            /**< Stable participant identity. */
        SaveVersionSupport<ParticipantSchemaVersionTag> versions; /**< Direct and migration ranges. */
        bool required{true};                                      /**< Required by current product composition. */
    };

    /** @brief Sealed release policy used for deterministic compatibility preflight. */
    struct SaveCompatibilityPolicy final {
        SaveVersionSupport<ArchiveFormatVersionTag> archiveVersions;            /**< Container version support. */
        SaveVersionSupport<SaveSchemaVersionTag> saveSchemaVersions;            /**< Whole-save schema support. */
        SaveVersionSupport<ProductSaveCompatibilityVersionTag> productVersions; /**< Product policy support. */
        std::vector<SaveParticipantCompatibility> participants;                 /**< Stable participant order. */
        std::uint64_t supportedFeatureFlagsMask{};                              /**< Understood header feature bits. */
    };

    /** @brief High-level action selected by compatibility preflight. */
    enum class SaveCompatibilityDisposition : std::uint8_t {
        DirectRead,
        MigrationRequired,
        Rejected,
    };

    /** @brief Stable reason suitable for release gates and presentation adapters. */
    enum class SaveCompatibilityReason : std::uint8_t {
        None,
        InvalidMetadata,
        UnsupportedArchiveVersion,
        UnsupportedSaveSchema,
        UnsupportedProductVersion,
        UnsupportedFeature,
        MissingRequiredParticipant,
        UnknownRequiredParticipant,
        UnsupportedParticipantSchema,
    };

    /** @brief Deterministic compatibility preflight outcome. */
    struct SaveCompatibilityDecision final {
        SaveCompatibilityDisposition disposition{SaveCompatibilityDisposition::Rejected}; /**< Required action. */
        SaveCompatibilityReason reason{SaveCompatibilityReason::InvalidMetadata};         /**< Stable rationale. */
        std::optional<SaveParticipantId> participant;                                     /**< Related participant, if any. */
    };

    /** @brief Validates a typed save header. @param header Header to validate. @param limits Decode limits and known flags.
     * @return Success or a stable metadata validation error.
     */
    [[nodiscard]] Result<void> ValidateSaveArchiveHeader(const SaveArchiveHeader &header, const SaveArchiveMetadataLimits &limits = {});
    /** @brief Validates canonical participant/chunk ordering and bounds. @param manifest Manifest to validate.
     * @param limits Decode limits. @return Success or a stable manifest validation error.
     */
    [[nodiscard]] Result<void> ValidateSaveGameManifest(const SaveGameManifest &manifest, const SaveArchiveMetadataLimits &limits = {});
    /** @brief Encodes canonical deterministic header JSON. @param header Valid header. @param limits Validation limits.
     * @return Canonical UTF-8 JSON or a typed validation error.
     */
    [[nodiscard]] Result<std::string> EncodeSaveArchiveHeader(const SaveArchiveHeader &header,
                                                              const SaveArchiveMetadataLimits &limits = {});
    /** @brief Decodes exact-shape canonical header JSON with duplicate-key rejection. @param json Untrusted UTF-8 JSON.
     * @param limits Admission limits. @return Owned validated header or a typed decode error.
     */
    [[nodiscard]] Result<SaveArchiveHeader> DecodeSaveArchiveHeader(std::string_view json, const SaveArchiveMetadataLimits &limits = {});
    /** @brief Encodes canonical deterministic manifest JSON. @param manifest Valid manifest. @param limits Validation limits.
     * @return Canonical UTF-8 JSON or a typed validation error.
     */
    [[nodiscard]] Result<std::string> EncodeSaveGameManifest(const SaveGameManifest &manifest,
                                                             const SaveArchiveMetadataLimits &limits = {});
    /** @brief Decodes exact-shape canonical manifest JSON with duplicate-key rejection. @param json Untrusted UTF-8 JSON.
     * @param limits Admission limits. @return Owned validated manifest or a typed decode error.
     */
    [[nodiscard]] Result<SaveGameManifest> DecodeSaveGameManifest(std::string_view json, const SaveArchiveMetadataLimits &limits = {});
    /** @brief Evaluates direct-read, migration, or rejection without decoding payloads. @param archiveVersion Container version.
     * @param header Validated header. @param manifest Validated manifest. @param policy Sealed release policy.
     * @return Stable compatibility disposition and reason.
     */
    [[nodiscard]] SaveCompatibilityDecision EvaluateSaveCompatibility(ArchiveFormatVersion archiveVersion, const SaveArchiveHeader &header,
                                                                      const SaveGameManifest &manifest,
                                                                      const SaveCompatibilityPolicy &policy);
}  // namespace Horo::Runtime
