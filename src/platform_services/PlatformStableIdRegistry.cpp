#include "Horo/PlatformServices/PlatformStableIdRegistry.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>

namespace Horo::PlatformServices {
    namespace {
        constexpr std::string_view AlgorithmDomain = "horo.platform-services.stable-id.v1";
        constexpr std::string_view FingerprintDomain = "horo.platform-services.registry-fingerprint.v2";
        constexpr std::size_t MaxEntries = 4096;
        constexpr std::size_t MaxAliasesPerEntry = 16;
        constexpr std::size_t MaxRemovalProvenanceBytes = 256;
        constexpr std::size_t MaxProviderMappings = 16384;

        [[nodiscard]] constexpr bool IsKnown(const PlatformServiceIdKind kind) noexcept {
            return kind >= PlatformServiceIdKind::Achievement && kind <= PlatformServiceIdKind::PresenceStatus;
        }

        [[nodiscard]] constexpr bool IsKnown(const PlatformStableIdState state) noexcept {
            return state == PlatformStableIdState::Active || state == PlatformStableIdState::Tombstoned;
        }

        [[nodiscard]] constexpr bool IsCanonicalKeyCharacter(const char character) noexcept {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_' ||
                   character == '.' || character == '-';
        }

        [[nodiscard]] bool IsCanonicalKey(const std::string_view key) noexcept {
            if (key.empty() || key.size() > 96 || key.front() < 'a' || key.front() > 'z')
                return false;
            return std::ranges::all_of(key.substr(1), IsCanonicalKeyCharacter);
        }

        [[nodiscard]] bool IsZero(const Sha256Digest &digest) noexcept {
            return std::ranges::all_of(digest.bytes, [](const std::uint8_t byte) {
                return byte == 0;
            });
        }

        void AppendString(std::vector<std::byte> &bytes, const std::string_view value) {
            bytes.push_back(static_cast<std::byte>((value.size() >> 8U) & 0xffU));
            bytes.push_back(static_cast<std::byte>(value.size() & 0xffU));
            for (const char character : value)
                bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
        }

        [[nodiscard]] Result<PlatformServiceStableIdValue> InvalidId(const ErrorCodeDescriptor &descriptor) {
            return Result<PlatformServiceStableIdValue>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] int HexValue(const char digit) noexcept {
            if (digit >= '0' && digit <= '9')
                return digit - '0';
            if (digit >= 'a' && digit <= 'f')
                return digit - 'a' + 10;
            return -1;
        }

        [[nodiscard]] std::size_t KindIndex(const PlatformServiceIdKind kind) noexcept {
            return static_cast<std::size_t>(kind) - 1U;
        }

        struct KeyOwner final {
            PlatformServiceIdKind kind;
            std::string_view key;
            PlatformStableIdState state;
        };

        [[nodiscard]] bool HasValidEntryShape(const PlatformStableIdDeclaration &entry) noexcept {
            return IsKnown(entry.kind) && IsKnown(entry.state) && IsCanonicalKey(entry.canonicalKey) &&
                   entry.aliases.size() <= MaxAliasesPerEntry && entry.removalProvenance.size() <= MaxRemovalProvenanceBytes &&
                   IsValidUtf8ScalarSequence(entry.removalProvenance);
        }

        [[nodiscard]] Result<void> CanonicalizeEntry(PlatformStableIdDeclaration &entry) {
            if (!HasValidEntryShape(entry))
                return Result<void>::Failure(MakeError(StableIdErrors::InvalidLedger));
            const bool activeHasRemoval = entry.state == PlatformStableIdState::Active && !entry.removalProvenance.empty();
            if (const bool tombstoneLacksRemoval = entry.state == PlatformStableIdState::Tombstoned && entry.removalProvenance.empty();
                activeHasRemoval || tombstoneLacksRemoval)
                return Result<void>::Failure(MakeError(StableIdErrors::InvalidLedger));
            std::ranges::sort(entry.aliases);
            if (std::ranges::any_of(entry.aliases, [](const auto &alias) {
                return !IsCanonicalKey(alias);
            }))
                return Result<void>::Failure(MakeError(StableIdErrors::InvalidKey));
            return Result<void>::Success();
        }

        [[nodiscard]] bool HasValidCandidateMetadata(const PlatformStableIdRegistryCandidate &candidate) noexcept {
            return candidate.schemaVersion == PlatformStableIdRegistrySchemaVersion && !candidate.projectId.empty() &&
                   candidate.projectId.size() <= PlatformServicesMaximumProjectIdBytes && IsValidUtf8ScalarSequence(candidate.projectId) &&
                   candidate.entries.size() <= MaxEntries;
        }

        [[nodiscard]] Result<void> ValidateNumericIds(const std::vector<PlatformStableIdDeclaration> &entries,
                                                      const PlatformServicesIdSalt &salt) {
            std::vector<const PlatformStableIdDeclaration *> numericIds;
            numericIds.reserve(entries.size());
            for (const auto &entry : entries)
                numericIds.push_back(&entry);
            std::ranges::sort(numericIds, [](const auto *left, const auto *right) {
                if (left->storedId != right->storedId)
                    return left->storedId < right->storedId;
                if (left->kind != right->kind)
                    return left->kind < right->kind;
                return left->canonicalKey < right->canonicalKey;
            });
            for (std::size_t index = 1; index < numericIds.size(); ++index) {
                const auto &left = *numericIds[index - 1];
                const auto &right = *numericIds[index];
                if (left.storedId != right.storedId)
                    continue;
                auto error = MakeError(StableIdErrors::HashCollision,
                                       std::string{AlgorithmDomain} + " collision at " + FormatPlatformServiceStableId(left.storedId));
                for (const auto *entry : {&left, &right})
                    error.diagnostics.push_back({.code = DiagnosticCode{StableIdErrors::HashCollision.code.Value()},
                                                 .severity = DiagnosticSeverity::Error,
                                                 .message = "Conflicting kind " + std::to_string(static_cast<unsigned>(entry->kind)) +
                                                            " primary " + entry->canonicalKey + "; " +
                                                            std::string{StableIdErrors::HashCollision.remediationHint},
                                                 .location = {.source = ".horo/platform_services.ids.json"},
                                                 .path = entry->canonicalKey});
                return Result<void>::Failure(std::move(error));
            }
            for (const auto &entry : entries) {
                const auto derived = DerivePlatformServiceStableId(salt, entry.kind, entry.canonicalKey);
                if (derived.HasError() || derived.Value() != entry.storedId)
                    return Result<void>::Failure(MakeError(StableIdErrors::StoredIdMismatch));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateKeys(const std::vector<PlatformStableIdDeclaration> &entries) {
            std::vector<KeyOwner> keys;
            keys.reserve(entries.size() * 2U);
            for (const auto &entry : entries) {
                keys.emplace_back(entry.kind, entry.canonicalKey, entry.state);
                for (const auto &alias : entry.aliases)
                    keys.emplace_back(entry.kind, alias, entry.state);
            }
            std::ranges::sort(keys, [](const KeyOwner &left, const KeyOwner &right) {
                if (left.kind != right.kind)
                    return left.kind < right.kind;
                return left.key < right.key;
            });
            for (std::size_t index = 1; index < keys.size(); ++index) {
                const auto &previous = keys[index - 1U];
                const auto &current = keys[index];
                if (previous.kind != current.kind || previous.key != current.key)
                    continue;
                const bool tombstoned =
                    previous.state == PlatformStableIdState::Tombstoned || current.state == PlatformStableIdState::Tombstoned;
                const auto &descriptor = tombstoned ? StableIdErrors::Tombstoned : StableIdErrors::DuplicateKey;
                auto error = MakeError(descriptor, "Kind " + std::to_string(static_cast<unsigned>(current.kind)) + " key " +
                                                       std::string{current.key} + ": " + std::string{descriptor.summary});
                error.diagnostics.push_back({.code = DiagnosticCode{descriptor.code.Value()},
                                             .message = std::string{descriptor.remediationHint},
                                             .location = {.source = ".horo/platform_services.ids.json"},
                                             .path = std::string{current.key}});
                return Result<void>::Failure(std::move(error));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Sha256Digest ComputeRegistryFingerprint(const std::string_view document) {
            constexpr std::array separator{std::byte{}};
            const std::array fragments{std::as_bytes(std::span{FingerprintDomain.data(), FingerprintDomain.size()}),
                                       std::span<const std::byte>{separator}, std::as_bytes(std::span{document.data(), document.size()})};
            return ComputeSha256Fragments(fragments);
        }

        [[nodiscard]] bool IsValidProviderMapping(const PlatformStableIdRegistry &registry, const PlatformProviderId provider,
                                                  const std::uint64_t expectedRevision,
                                                  const PlatformProviderMappingEvidence &mapping) noexcept {
            return mapping.provider == provider && mapping.registryFingerprint == registry.Fingerprint() && mapping.mappingRevision != 0 &&
                   mapping.mappingRevision == expectedRevision && !IsZero(mapping.providerValueDigest) &&
                   registry.ContainsActive(mapping.kind, mapping.id);
        }

        [[nodiscard]] bool HasDuplicates(std::vector<PlatformServiceStableIdValue> &mappedIds, std::vector<Sha256Digest> &providerValues) {
            std::ranges::sort(mappedIds);
            std::ranges::sort(providerValues);
            return std::ranges::adjacent_find(mappedIds) != mappedIds.end() ||
                   std::ranges::adjacent_find(providerValues) != providerValues.end();
        }

        [[nodiscard]] bool HasAllRequiredMappings(const PlatformStableIdRegistry &registry, const PlatformProviderMappingPolicy &policy,
                                                  const std::vector<PlatformServiceStableIdValue> &mappedIds) {
            return std::ranges::all_of(registry.Entries(), [&](const auto &entry) {
                return entry.state != PlatformStableIdState::Active || !policy.requiredKinds[KindIndex(entry.kind)] ||
                       std::ranges::binary_search(mappedIds, entry.storedId);
            });
        }
    }  // namespace

    namespace StableIdErrors {
        namespace {
            const ErrorDomainId Domain{"horo.platform.identity"};
        }

        const ErrorCodeDescriptor InvalidSalt{Domain,
                                              ErrorCode{"platform.identity.invalid_salt"},
                                              ErrorSeverity::Error,
                                              "Platform Services ID salt is invalid.",
                                              "Provide one canonical nonzero 128-bit project salt.",
                                              false,
                                              false};
        const ErrorCodeDescriptor InvalidKey{Domain,
                                             ErrorCode{"platform.identity.invalid_key"},
                                             ErrorSeverity::Error,
                                             "Platform Services identity key is invalid.",
                                             "Use 1-96 lowercase ASCII machine-identity bytes.",
                                             false,
                                             false};
        const ErrorCodeDescriptor InvalidLedger{Domain,
                                                ErrorCode{"platform.identity.invalid_ledger"},
                                                ErrorSeverity::Error,
                                                "Platform Services identity ledger is invalid.",
                                                "Correct the bounded versioned project ledger.",
                                                false,
                                                false};
        const ErrorCodeDescriptor StoredIdMismatch{Domain,
                                                   ErrorCode{"platform.identity.stored_id_mismatch"},
                                                   ErrorSeverity::Error,
                                                   "Stored Platform Services ID does not match deterministic derivation.",
                                                   "Restore the matching committed project salt and ledger; never rewrite IDs as a repair.",
                                                   false,
                                                   false};
        const ErrorCodeDescriptor HashCollision{Domain,
                                                ErrorCode{"platform.identity.hash_collision"},
                                                ErrorSeverity::Critical,
                                                "Two Platform Services definitions share one numeric identity.",
                                                "Choose a new unpublished canonical key or perform an explicit migration.",
                                                false,
                                                false};
        const ErrorCodeDescriptor DuplicateKey{Domain,
                                               ErrorCode{"platform.identity.duplicate_key"},
                                               ErrorSeverity::Error,
                                               "A Platform Services primary key or alias is duplicated.",
                                               "Keep every key unique within its identity kind.",
                                               false,
                                               false};
        const ErrorCodeDescriptor Tombstoned{Domain,
                                             ErrorCode{"platform.identity.tombstoned"},
                                             ErrorSeverity::Error,
                                             "The requested Platform Services identity is permanently tombstoned.",
                                             "Use an explicit compatible restore or choose a new semantic key.",
                                             false,
                                             false};
        const ErrorCodeDescriptor UnknownIdentity{Domain,
                                                  ErrorCode{"platform.identity.unknown"},
                                                  ErrorSeverity::Error,
                                                  "The Platform Services identity is not registered.",
                                                  "Reference an active registry definition.",
                                                  false,
                                                  false};
        const ErrorCodeDescriptor InvalidProviderMapping{Domain,
                                                         ErrorCode{"platform.identity.invalid_provider_mapping"},
                                                         ErrorSeverity::Error,
                                                         "Provider mapping evidence is incomplete, stale, or ambiguous.",
                                                         "Regenerate mappings from the captured registry generation.",
                                                         false,
                                                         false};
        const ErrorCodeDescriptor StaleReplacement{Domain,
                                                   ErrorCode{"platform.identity.draft_stale"},
                                                   ErrorSeverity::Error,
                                                   "The Platform Services ledger changed after this edit began.",
                                                   "Reload the current ledger and review the edit before retrying.",
                                                   false,
                                                   true};
        const ErrorCodeDescriptor
            InvalidTransition{Domain,
                              ErrorCode{"platform.identity.invalid_transition"},
                              ErrorSeverity::Error,
                              "The edit would discard or reinterpret Platform Services identity history.",
                              "Preserve primaries, aliases and tombstones; use an explicit migration for namespace changes.",
                              false,
                              true};
    }  // namespace StableIdErrors

    /** @copydoc PlatformServicesIdSalt::IsValid */
    bool PlatformServicesIdSalt::IsValid() const noexcept {
        return std::ranges::any_of(bytes, [](const std::byte byte) {
            return byte != std::byte{};
        });
    }

    /** @copydoc DerivePlatformServiceStableId */
    Result<PlatformServiceStableIdValue> DerivePlatformServiceStableId(const PlatformServicesIdSalt &salt, const PlatformServiceIdKind kind,
                                                                       const std::string_view canonicalKey) {
        if (!salt.IsValid())
            return InvalidId(StableIdErrors::InvalidSalt);
        if (!IsKnown(kind) || !IsCanonicalKey(canonicalKey))
            return InvalidId(StableIdErrors::InvalidKey);
        std::vector<std::byte> preimage;
        preimage.reserve(AlgorithmDomain.size() + 20U + canonicalKey.size());
        for (const char character : AlgorithmDomain)
            preimage.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
        preimage.push_back(std::byte{});
        preimage.insert(preimage.end(), salt.bytes.begin(), salt.bytes.end());
        preimage.push_back(static_cast<std::byte>(kind));
        AppendString(preimage, canonicalKey);
        const auto digest = ComputeSha256(std::span<const std::byte>{preimage});
        std::uint64_t value{};
        for (std::size_t index = 0; index < 8; ++index)
            value = (value << 8U) | digest.bytes[index];
        if (value == 0)
            return InvalidId(StableIdErrors::HashCollision);
        return Result<PlatformServiceStableIdValue>::Success({value});
    }

    /** @copydoc BuildPlatformStableIdRegistry */
    Result<PlatformStableIdRegistry> BuildPlatformStableIdRegistry(const PlatformStableIdRegistryCandidate &candidate) {
        if (!candidate.salt.IsValid())
            return Result<PlatformStableIdRegistry>::Failure(MakeError(StableIdErrors::InvalidSalt));
        if (!HasValidCandidateMetadata(candidate))
            return Result<PlatformStableIdRegistry>::Failure(MakeError(StableIdErrors::InvalidLedger));

        PlatformStableIdRegistry registry;
        registry.projectId_ = candidate.projectId;
        registry.salt_ = candidate.salt;
        registry.entries_ = candidate.entries;
        std::ranges::sort(registry.entries_, [](const auto &left, const auto &right) {
            if (left.kind != right.kind)
                return left.kind < right.kind;
            return left.storedId < right.storedId;
        });

        for (auto &entry : registry.entries_) {
            const auto canonicalized = CanonicalizeEntry(entry);
            if (canonicalized.HasError())
                return Result<PlatformStableIdRegistry>::Failure(canonicalized.ErrorValue());
        }
        if (const auto keys = ValidateKeys(registry.entries_); keys.HasError())
            return Result<PlatformStableIdRegistry>::Failure(keys.ErrorValue());
        if (const auto numericIds = ValidateNumericIds(registry.entries_, candidate.salt); numericIds.HasError())
            return Result<PlatformStableIdRegistry>::Failure(numericIds.ErrorValue());
        const auto document = registry.CanonicalDocument();
        if (document.HasError())
            return Result<PlatformStableIdRegistry>::Failure(document.ErrorValue());
        registry.fingerprint_ = ComputeRegistryFingerprint(document.Value());
        return Result<PlatformStableIdRegistry>::Success(std::move(registry));
    }

    /** @copydoc BuildPlatformStableIdRegistryReplacement */
    Result<PlatformStableIdRegistry> BuildPlatformStableIdRegistryReplacement(const PlatformStableIdRegistry &previous,
                                                                              const Sha256Digest &baseFingerprint,
                                                                              const PlatformStableIdRegistryCandidate &candidate) {
        using Output = Result<PlatformStableIdRegistry>;
        if (baseFingerprint != previous.Fingerprint())
            return Output::Failure(MakeError(StableIdErrors::StaleReplacement));
        if (candidate.projectId != previous.projectId_ || candidate.salt != previous.salt_)
            return Output::Failure(MakeError(StableIdErrors::InvalidTransition));
        auto replacement = BuildPlatformStableIdRegistry(candidate);
        if (replacement.HasError())
            return replacement;
        const auto entries = replacement.Value().Entries();
        for (const auto &old : previous.Entries()) {
            const auto found = std::ranges::lower_bound(entries, old, [](const auto &left, const auto &right) {
                if (left.kind != right.kind)
                    return left.kind < right.kind;
                return left.storedId < right.storedId;
            });
            if (found == entries.end() || found->kind != old.kind || found->storedId != old.storedId ||
                found->canonicalKey != old.canonicalKey || !std::ranges::includes(found->aliases, old.aliases))
                return Output::Failure(MakeError(StableIdErrors::InvalidTransition));
            if (old.state == PlatformStableIdState::Tombstoned &&
                (found->state != PlatformStableIdState::Tombstoned || found->removalProvenance != old.removalProvenance))
                return Output::Failure(MakeError(StableIdErrors::Tombstoned));
        }
        return replacement;
    }

    /** @copydoc PlatformStableIdRegistry::ProjectId */
    std::string_view PlatformStableIdRegistry::ProjectId() const noexcept {
        return projectId_;
    }

    /** @copydoc PlatformStableIdRegistry::Fingerprint */
    const Sha256Digest &PlatformStableIdRegistry::Fingerprint() const noexcept {
        return fingerprint_;
    }

    /** @copydoc PlatformStableIdRegistry::Entries */
    std::span<const PlatformStableIdDeclaration> PlatformStableIdRegistry::Entries() const noexcept {
        return entries_;
    }

    /** @copydoc PlatformStableIdRegistry::Resolve */
    Result<PlatformServiceStableIdValue> PlatformStableIdRegistry::Resolve(const PlatformServiceIdKind kind,
                                                                           const std::string_view key) const {
        if (!IsKnown(kind) || !IsCanonicalKey(key))
            return InvalidId(StableIdErrors::InvalidKey);
        for (const auto &entry : entries_) {
            if (entry.kind != kind || (entry.canonicalKey != key && std::ranges::find(entry.aliases, key) == entry.aliases.end()))
                continue;
            if (entry.state == PlatformStableIdState::Tombstoned)
                return InvalidId(StableIdErrors::Tombstoned);
            return Result<PlatformServiceStableIdValue>::Success(entry.storedId);
        }
        return InvalidId(StableIdErrors::UnknownIdentity);
    }

    namespace {
        template <typename StableId>
        [[nodiscard]] Result<StableId> ResolveTyped(const PlatformStableIdRegistry &registry, const PlatformServiceIdKind kind,
                                                    const std::string_view key) {
            const auto resolved = registry.Resolve(kind, key);
            if (resolved.HasError())
                return Result<StableId>::Failure(resolved.ErrorValue());
            return Result<StableId>::Success({resolved.Value().value});
        }
    }  // namespace

    /** @copydoc PlatformStableIdRegistry::ResolveAchievement */
    Result<AchievementId> PlatformStableIdRegistry::ResolveAchievement(const std::string_view key) const {
        return ResolveTyped<AchievementId>(*this, PlatformServiceIdKind::Achievement, key);
    }

    /** @copydoc PlatformStableIdRegistry::ResolveLeaderboard */
    Result<LeaderboardId> PlatformStableIdRegistry::ResolveLeaderboard(const std::string_view key) const {
        return ResolveTyped<LeaderboardId>(*this, PlatformServiceIdKind::Leaderboard, key);
    }

    /** @copydoc PlatformStableIdRegistry::ResolveStat */
    Result<StatId> PlatformStableIdRegistry::ResolveStat(const std::string_view key) const {
        return ResolveTyped<StatId>(*this, PlatformServiceIdKind::Stat, key);
    }

    /** @copydoc PlatformStableIdRegistry::ResolvePresenceStatus */
    Result<PresenceStatusId> PlatformStableIdRegistry::ResolvePresenceStatus(const std::string_view key) const {
        return ResolveTyped<PresenceStatusId>(*this, PlatformServiceIdKind::PresenceStatus, key);
    }

    /** @copydoc PlatformStableIdRegistry::ContainsActive */
    bool PlatformStableIdRegistry::ContainsActive(const PlatformServiceIdKind kind, const PlatformServiceStableIdValue id) const noexcept {
        if (!IsKnown(kind) || !id.IsValid())
            return false;
        const auto found = std::ranges::lower_bound(entries_, std::pair{kind, id}, {}, [](const auto &entry) {
            return std::pair{entry.kind, entry.storedId};
        });
        return found != entries_.end() && found->kind == kind && found->storedId == id && found->state == PlatformStableIdState::Active;
    }

    /** @copydoc ValidatePlatformProviderMappings */
    Result<void> ValidatePlatformProviderMappings(const PlatformStableIdRegistry &registry, const PlatformProviderId provider,
                                                  const PlatformProviderMappingPolicy &policy,
                                                  const std::span<const PlatformProviderMappingEvidence> mappings) {
        if (!provider.IsValid() || mappings.size() > MaxProviderMappings)
            return Result<void>::Failure(MakeError(StableIdErrors::InvalidProviderMapping));
        std::vector<PlatformServiceStableIdValue> mappedIds;
        std::vector<Sha256Digest> providerValues;
        const std::uint64_t expectedRevision = mappings.empty() ? 0 : mappings.front().mappingRevision;
        mappedIds.reserve(mappings.size());
        providerValues.reserve(mappings.size());
        for (const auto &mapping : mappings) {
            if (!IsValidProviderMapping(registry, provider, expectedRevision, mapping))
                return Result<void>::Failure(MakeError(StableIdErrors::InvalidProviderMapping));
            mappedIds.push_back(mapping.id);
            providerValues.push_back(mapping.providerValueDigest);
        }
        if (HasDuplicates(mappedIds, providerValues))
            return Result<void>::Failure(MakeError(StableIdErrors::InvalidProviderMapping));
        if (!HasAllRequiredMappings(registry, policy, mappedIds))
            return Result<void>::Failure(MakeError(StableIdErrors::InvalidProviderMapping));
        return Result<void>::Success();
    }

    /** @copydoc FormatPlatformServicesIdSalt */
    std::string FormatPlatformServicesIdSalt(const PlatformServicesIdSalt &salt) {
        constexpr char Hex[] = "0123456789abcdef";
        std::string result = "psid1:";
        result.reserve(38);
        for (const auto byte : salt.bytes) {
            const auto value = std::to_integer<unsigned>(byte);
            result.push_back(Hex[value >> 4U]);
            result.push_back(Hex[value & 0x0fU]);
        }
        return result;
    }

    /** @copydoc ParsePlatformServicesIdSalt */
    Result<PlatformServicesIdSalt> ParsePlatformServicesIdSalt(const std::string_view text) {
        if (!text.starts_with("psid1:") || text.size() != 38)
            return Result<PlatformServicesIdSalt>::Failure(MakeError(StableIdErrors::InvalidSalt));
        PlatformServicesIdSalt salt;
        for (std::size_t index = 0; index < salt.bytes.size(); ++index) {
            const int high = HexValue(text[6U + index * 2U]);
            const int low = HexValue(text[7U + index * 2U]);
            if (high < 0 || low < 0)
                return Result<PlatformServicesIdSalt>::Failure(MakeError(StableIdErrors::InvalidSalt));
            salt.bytes[index] = static_cast<std::byte>((high << 4) | low);
        }
        if (!salt.IsValid())
            return Result<PlatformServicesIdSalt>::Failure(MakeError(StableIdErrors::InvalidSalt));
        return Result<PlatformServicesIdSalt>::Success(salt);
    }

    /** @copydoc FormatPlatformServiceStableId */
    std::string FormatPlatformServiceStableId(const PlatformServiceStableIdValue id) {
        constexpr char Hex[] = "0123456789abcdef";
        std::string result = "sid1:";
        result.reserve(21);
        for (int shift = 60; shift >= 0; shift -= 4)
            result.push_back(Hex[(id.value >> shift) & 0x0fU]);
        return result;
    }

    /** @copydoc ParsePlatformServiceStableId */
    Result<PlatformServiceStableIdValue> ParsePlatformServiceStableId(const std::string_view text) {
        if (!text.starts_with("sid1:") || text.size() != 21)
            return InvalidId(StableIdErrors::InvalidLedger);
        std::uint64_t value{};
        for (const char digit : text.substr(5)) {
            const int nibble = HexValue(digit);
            if (nibble < 0)
                return InvalidId(StableIdErrors::InvalidLedger);
            value = (value << 4U) | static_cast<unsigned>(nibble);
        }
        if (value == 0)
            return InvalidId(StableIdErrors::InvalidLedger);
        return Result<PlatformServiceStableIdValue>::Success({value});
    }
}  // namespace Horo::PlatformServices
