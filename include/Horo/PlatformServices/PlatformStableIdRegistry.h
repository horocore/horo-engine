#pragma once

/**
 * @file PlatformStableIdRegistry.h
 * @brief Deterministic project-salted Platform Services identity registry contract.
 */

#include "Horo/Foundation/Sha256.h"
#include "Horo/PlatformServices/PlatformServiceInterfaces.h"
#include "Horo/PlatformServices/PlatformServicesBackend.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::PlatformServices {
    inline constexpr std::uint32_t PlatformStableIdRegistrySchemaVersion = 1;
    /** @brief Hard byte limit shared by canonical ledger encoding and untrusted document loading. */
    inline constexpr std::size_t PlatformStableIdRegistryMaximumDocumentBytes = 4U * 1024U * 1024U;

    /** @brief Disjoint authoring identity kinds covered by registry schema version 1. */
    enum class PlatformServiceIdKind : std::uint8_t {
        Achievement = 1,
        Leaderboard = 2,
        Stat = 3,
        PresenceStatus = 4
    };

    /** @brief Durable definition lifecycle; tombstones permanently reserve their identity. */
    enum class PlatformStableIdState : std::uint8_t {
        Active,
        Tombstoned
    };

    /** @brief Exact non-secret 128-bit project namespace salt. */
    struct PlatformServicesIdSalt final {
        std::array<std::byte, 16> bytes{}; /**< Exact raw namespace bytes; all-zero is invalid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const PlatformServicesIdSalt &) const noexcept = default;
    };

    /** @brief Untyped transport form used only by generic registry tooling. */
    struct PlatformServiceStableIdValue final {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformServiceStableIdValue &) const noexcept = default;
    };

    /** @brief One complete ledger declaration copied into candidate validation. */
    struct PlatformStableIdDeclaration final {
        PlatformServiceIdKind kind{PlatformServiceIdKind::Achievement}; /**< Disjoint typed identity kind. */
        std::string canonicalKey;                                       /**< Immutable schema-v1 derivation key. */
        PlatformServiceStableIdValue storedId;                          /**< Reviewed derived numeric value. */
        std::vector<std::string> aliases;                               /**< Direct authoring aliases; never provider names. */
        PlatformStableIdState state{PlatformStableIdState::Active};     /**< Active or permanently reserved. */
        std::string removalProvenance;                                  /**< Required bounded evidence for tombstones only. */
    };

    /** @brief Bounded ledger candidate owned by the project domain. */
    struct PlatformStableIdRegistryCandidate final {
        std::uint32_t schemaVersion{PlatformStableIdRegistrySchemaVersion}; /**< Exact ledger schema version. */
        std::string projectId;                                              /**< Authoritative root project identity. */
        PlatformServicesIdSalt salt;                                        /**< Authoritative root project namespace salt. */
        std::vector<PlatformStableIdDeclaration> entries;                   /**< Detached complete ledger content. */
    };

    /** @brief Opaque provider-side mapping evidence; provider-native values remain private. */
    struct PlatformProviderMappingEvidence final {
        PlatformProviderId provider;                                    /**< Selected Horo provider identity. */
        PlatformServiceIdKind kind{PlatformServiceIdKind::Achievement}; /**< Expected registry kind. */
        PlatformServiceStableIdValue id;                                /**< Active Horo identity being mapped. */
        Sha256Digest providerValueDigest{};                             /**< Opaque duplicate-detection evidence. */
        Sha256Digest registryFingerprint{};                             /**< Exact captured registry fingerprint. */
        std::uint64_t mappingRevision{};                                /**< Nonzero coherent mapping generation. */
    };

    /** @brief Product policy controlling which active kinds require provider mappings. */
    struct PlatformProviderMappingPolicy final {
        std::array<bool, 4> requiredKinds{};
    };

    /** @brief Stable failures emitted by registry and provider-mapping validation. */
    namespace StableIdErrors {
        extern const ErrorCodeDescriptor InvalidSalt;
        extern const ErrorCodeDescriptor InvalidKey;
        extern const ErrorCodeDescriptor InvalidLedger;
        extern const ErrorCodeDescriptor StoredIdMismatch;
        extern const ErrorCodeDescriptor HashCollision;
        extern const ErrorCodeDescriptor DuplicateKey;
        extern const ErrorCodeDescriptor Tombstoned;
        extern const ErrorCodeDescriptor UnknownIdentity;
        extern const ErrorCodeDescriptor InvalidProviderMapping;
        extern const ErrorCodeDescriptor StaleReplacement;
        extern const ErrorCodeDescriptor InvalidTransition;
    }  // namespace StableIdErrors

    /** @brief Immutable sorted registry generation safe to retain across request lifetimes. */
    class PlatformStableIdRegistry final {
    public:
        /** @brief Returns owning project identity. @return Borrowed bounded project ID. */
        [[nodiscard]] std::string_view ProjectId() const noexcept;
        /** @brief Returns canonical ledger fingerprint. @return Borrowed SHA-256 digest. */
        [[nodiscard]] const Sha256Digest &Fingerprint() const noexcept;
        /** @brief Returns the complete canonical ledger bytes used by the fingerprint.
         * @return Bounded two-space-indented UTF-8 JSON with one final newline, or invalid-ledger failure for an uninitialized snapshot.
         * @details Field order is schemaVersion, projectId, algorithm, entries; entries retain kind/ID order and aliases retain ASCII
         * order. Salt remains solely in root project metadata. Fingerprints use the v2 domain, one zero byte and these exact document
         * bytes.
         */
        [[nodiscard]] Result<std::string> CanonicalDocument() const;
        /** @brief Returns sorted immutable declarations. @return Borrowed complete entry span. */
        [[nodiscard]] std::span<const PlatformStableIdDeclaration> Entries() const noexcept;
        /** @brief Resolves an active primary key or direct alias. @param kind Expected typed kind. @param key Canonical key bytes. @return
         * Stable ID or typed unknown/tombstone failure. */
        [[nodiscard]] Result<PlatformServiceStableIdValue> Resolve(PlatformServiceIdKind kind, std::string_view key) const;
        /** @brief Resolves an achievement key. @param key Canonical key or alias. @return Strong achievement ID or typed failure. */
        [[nodiscard]] Result<AchievementId> ResolveAchievement(std::string_view key) const;
        /** @brief Resolves a leaderboard key. @param key Canonical key or alias. @return Strong leaderboard ID or typed failure. */
        [[nodiscard]] Result<LeaderboardId> ResolveLeaderboard(std::string_view key) const;
        /** @brief Resolves a statistic key. @param key Canonical key or alias. @return Strong statistic ID or typed failure. */
        [[nodiscard]] Result<StatId> ResolveStat(std::string_view key) const;
        /** @brief Resolves a presence-status key. @param key Canonical key or alias. @return Strong presence-status ID or typed failure. */
        [[nodiscard]] Result<PresenceStatusId> ResolvePresenceStatus(std::string_view key) const;
        /** @brief Tests whether an active definition owns an exact typed ID. @param kind Expected kind. @param id Candidate ID. @return
         * Whether an active exact match exists. */
        [[nodiscard]] bool ContainsActive(PlatformServiceIdKind kind, PlatformServiceStableIdValue id) const noexcept;

    private:
        friend Result<PlatformStableIdRegistry> BuildPlatformStableIdRegistry(const PlatformStableIdRegistryCandidate &);
        friend Result<PlatformStableIdRegistry> BuildPlatformStableIdRegistryReplacement(const PlatformStableIdRegistry &,
                                                                                         const Sha256Digest &,
                                                                                         const PlatformStableIdRegistryCandidate &);
        std::string projectId_;
        PlatformServicesIdSalt salt_; /**< Captured root authority used only to fence ordinary replacements. */
        std::vector<PlatformStableIdDeclaration> entries_;
        Sha256Digest fingerprint_{};
    };

    /** @brief Derives schema-v1 ID bytes. @param salt Valid 128-bit project salt. @param kind Stable kind tag. @param canonicalKey Exact
     * canonical key. @return Derived nonzero ID or typed validation failure. */
    [[nodiscard]] Result<PlatformServiceStableIdValue> DerivePlatformServiceStableId(const PlatformServicesIdSalt &salt,
                                                                                     PlatformServiceIdKind kind,
                                                                                     std::string_view canonicalKey);
    /** @brief Builds and validates one immutable registry. @param candidate Detached complete ledger candidate. @return Sorted registry or
     * typed fail-closed diagnostic. */
    [[nodiscard]] Result<PlatformStableIdRegistry> BuildPlatformStableIdRegistry(const PlatformStableIdRegistryCandidate &candidate);
    /**
     * @brief Validates an ordinary ledger edit against its captured immutable generation.
     * @param previous Current registry at the host's serialized publication boundary.
     * @param baseFingerprint Exact generation captured when the edit began.
     * @param candidate Complete detached ledger using the unchanged root project identity and salt.
     * @return Compatible replacement or typed failure, leaving previous untouched.
     * @details Every prior primary, alias and tombstone must survive. Active entries may become tombstones;
     * restoration, salt regeneration and identity forks require a separate explicit migration operation.
     * The host must serialize this check with publication and durable persistence; validation alone does not write files.
     */
    [[nodiscard]] Result<PlatformStableIdRegistry> BuildPlatformStableIdRegistryReplacement(
        const PlatformStableIdRegistry &previous, const Sha256Digest &baseFingerprint, const PlatformStableIdRegistryCandidate &candidate);
    /** @brief Validates provider mapping evidence against one registry generation. @param registry Captured registry. @param provider
     * Selected Horo provider. @param policy Required-kind policy. @param mappings Bounded opaque mapping evidence. @return Success or typed
     * stale/unknown/tombstoned/duplicate failure. */
    [[nodiscard]] Result<void> ValidatePlatformProviderMappings(const PlatformStableIdRegistry &registry, PlatformProviderId provider,
                                                                const PlatformProviderMappingPolicy &policy,
                                                                std::span<const PlatformProviderMappingEvidence> mappings);
    /** @brief Formats canonical salt text. @param salt Valid salt. @return `psid1:` plus 32 lowercase hex digits. */
    [[nodiscard]] std::string FormatPlatformServicesIdSalt(const PlatformServicesIdSalt &salt);
    /** @brief Parses canonical salt text. @param text Exact canonical representation. @return Valid nonzero salt or typed failure. */
    [[nodiscard]] Result<PlatformServicesIdSalt> ParsePlatformServicesIdSalt(std::string_view text);
    /** @brief Formats canonical stable-ID text. @param id Valid ID. @return `sid1:` plus 16 lowercase hex digits. */
    [[nodiscard]] std::string FormatPlatformServiceStableId(PlatformServiceStableIdValue id);
    /** @brief Parses canonical stable-ID text. @param text Exact canonical representation. @return Valid nonzero ID or typed failure. */
    [[nodiscard]] Result<PlatformServiceStableIdValue> ParsePlatformServiceStableId(std::string_view text);
}  // namespace Horo::PlatformServices
