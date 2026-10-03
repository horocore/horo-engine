#include "Horo/PlatformServices/PlatformProviderManifestCook.h"

#include "PlatformDefinitionRegistryDetail.h"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <string_view>
#include <utility>

namespace Horo::PlatformServices {
    namespace {
        using DefinitionRegistryDetail::MakeDescriptor;
        using DefinitionRegistryDetail::MakeDiagnostic;

        constexpr std::size_t MaximumMappings = 16384;
        constexpr std::size_t MaximumSourceBytes = 256;

        class Writer final {
        public:
            void Byte(const std::uint8_t value) {
                bytes_.push_back(static_cast<std::byte>(value));
            }

            void U32(const std::uint32_t value) {
                for (int shift = 24; shift >= 0; shift -= 8)
                    Byte(static_cast<std::uint8_t>(value >> shift));
            }

            void U64(const std::uint64_t value) {
                for (int shift = 56; shift >= 0; shift -= 8)
                    Byte(static_cast<std::uint8_t>(value >> shift));
            }

            void Text(const std::string_view value) {
                U32(static_cast<std::uint32_t>(value.size()));
                for (const unsigned char character : value)
                    Byte(character);
            }

            void Digest(const Sha256Digest &digest) {
                for (const auto byte : digest.bytes)
                    Byte(byte);
            }

            void Bytes(const std::span<const std::byte> value) {
                bytes_.insert(bytes_.end(), value.begin(), value.end());
            }

            [[nodiscard]] std::vector<std::byte> Take() && {
                return std::move(bytes_);
            }

        private:
            std::vector<std::byte> bytes_;
        };

        [[nodiscard]] bool IsCancelled(const PlatformProviderManifestCookInput &input) noexcept {
            return input.cancellation.stop_requested();
        }

        [[nodiscard]] bool IsCoherent(const PlatformProviderManifestCookInput &input) noexcept {
            const auto &ids = input.stableIds;
            const auto matches = [&ids](const auto &definitions) {
                return definitions.StableIdProjectId() == ids.ProjectId() && definitions.StableIdRegistryFingerprint() == ids.Fingerprint();
            };
            return input.configuration.ProjectId() == ids.ProjectId() && matches(input.achievements) && matches(input.leaderboards) &&
                   matches(input.stats) && matches(input.presence);
        }

        [[nodiscard]] PlatformServiceKind ServiceFor(const PlatformServiceIdKind kind) noexcept {
            using enum PlatformServiceIdKind;
            using enum PlatformServiceKind;
            switch (kind) {
                case Achievement:
                    return Achievements;
                case Leaderboard:
                case Stat:
                    return LeaderboardsAndStats;
                case PresenceStatus:
                    return Presence;
            }
            return Count;
        }

        [[nodiscard]] bool RequiredPolicyIsCoherent(const PlatformProviderManifestCookInput &input) noexcept {
            const auto requirements = input.configuration.ServiceRequirements();
            for (std::size_t index = 0; index < input.mappingPolicy.requiredKinds.size(); ++index) {
                const auto service = ServiceFor(static_cast<PlatformServiceIdKind>(index + 1U));
                const auto requirement = requirements[static_cast<std::size_t>(service)];
                if (requirement == PlatformServiceRequirement::Required && !input.mappingPolicy.requiredKinds[index])
                    return false;
                if (requirement == PlatformServiceRequirement::Disabled && input.mappingPolicy.requiredKinds[index])
                    return false;
            }
            return true;
        }

        [[nodiscard]] std::string LedgerSource(const PlatformStableIdDeclaration &entry) {
            return std::format("platform_services.ids.json:{}:{}", static_cast<unsigned>(entry.kind), entry.canonicalKey);
        }

        [[nodiscard]] Result<void> CheckMappingRow(const PlatformProviderManifestCookInput &input, const PlatformProviderId selected,
                                                   const PlatformCookMapping &row) {
            if (row.source.empty() || row.source.size() > MaximumSourceBytes)
                return Result<void>::Failure(MakeDiagnostic(PlatformProviderManifestCookErrors::InvalidInput, "mappings.source",
                                                            "Mapping source must be nonempty and bounded."));
            const auto &mapping = row.evidence;
            if (mapping.mappingRevision != input.mappingRevision || mapping.provider != selected ||
                mapping.registryFingerprint != input.stableIds.Fingerprint())
                return Result<void>::Failure(
                    MakeDiagnostic(PlatformProviderManifestCookErrors::StaleGeneration, row.source,
                                   "Mapping provider, ledger fingerprint or revision differs from the cook capture."));
            if (mapping.kind < PlatformServiceIdKind::Achievement || mapping.kind > PlatformServiceIdKind::PresenceStatus ||
                input.configuration.ServiceRequirements()[static_cast<std::size_t>(ServiceFor(mapping.kind))] ==
                    PlatformServiceRequirement::Disabled ||
                !input.stableIds.ContainsActive(mapping.kind, mapping.id))
                return Result<void>::Failure(MakeDiagnostic(PlatformProviderManifestCookErrors::InvalidMapping, row.source,
                                                            "Mapping targets a disabled, unknown or tombstoned identity."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CheckRequiredMappings(const PlatformProviderManifestCookInput &input,
                                                         const std::span<const PlatformProviderMappingEvidence> sorted) {
            for (const auto &entry : input.stableIds.Entries()) {
                if (IsCancelled(input))
                    return Result<void>::Failure(MakeError(PlatformProviderManifestCookErrors::Cancelled));
                if (entry.state != PlatformStableIdState::Active ||
                    !input.mappingPolicy.requiredKinds[static_cast<std::size_t>(entry.kind) - 1U])
                    continue;
                const auto found = std::ranges::lower_bound(sorted, std::pair{entry.kind, entry.storedId}, {}, [](const auto &mapping) {
                    return std::pair{mapping.kind, mapping.id};
                });
                if (found == sorted.end() || found->kind != entry.kind || found->id != entry.storedId)
                    return Result<void>::Failure(MakeDiagnostic(PlatformProviderManifestCookErrors::MissingMapping, LedgerSource(entry),
                                                                "Required active identity has no provider mapping."));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CheckMappingSelection(const PlatformProviderManifestCookInput &input) {
            if (input.mappings.size() > MaximumMappings)
                return Result<void>::Failure(MakeDiagnostic(PlatformProviderManifestCookErrors::InvalidInput, "mappings",
                                                            "Mapping count exceeds the finite cook bound."));
            if (!input.configuration.SelectedProvider()) {
                if (input.mappingRevision != 0 || !input.mappings.empty() ||
                    std::ranges::any_of(input.mappingPolicy.requiredKinds, [](const bool required) {
                    return required;
                }))
                    return Result<void>::Failure(MakeDiagnostic(PlatformProviderManifestCookErrors::InvalidInput, "provider",
                                                                "Explicit Null cannot carry a provider mapping generation."));
                return Result<void>::Success();
            }
            if (input.mappingRevision == 0)
                return Result<void>::Failure(MakeDiagnostic(PlatformProviderManifestCookErrors::InvalidInput, "mappingRevision",
                                                            "Exact provider cook requires a nonzero mapping revision."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateMappings(const PlatformProviderManifestCookInput &input,
                                                    std::vector<PlatformProviderMappingEvidence> &sorted) {
            if (const auto selected = CheckMappingSelection(input); selected.HasError())
                return selected;
            const auto provider = input.configuration.SelectedProvider();
            if (!provider)
                return Result<void>::Success();

            sorted.reserve(input.mappings.size());
            for (const auto &row : input.mappings) {
                if (IsCancelled(input))
                    return Result<void>::Failure(MakeError(PlatformProviderManifestCookErrors::Cancelled));
                if (const auto checked = CheckMappingRow(input, *provider, row); checked.HasError())
                    return checked;
                sorted.push_back(row.evidence);
            }
            std::ranges::sort(sorted, [](const auto &left, const auto &right) {
                if (left.kind != right.kind)
                    return left.kind < right.kind;
                return left.id < right.id;
            });
            if (const auto required = CheckRequiredMappings(input, sorted); required.HasError())
                return required;
            if (const auto validation = ValidatePlatformProviderMappings(input.stableIds, *provider, input.mappingPolicy, sorted);
                validation.HasError())
                return Result<void>::Failure(MakeDiagnostic(PlatformProviderManifestCookErrors::InvalidMapping, "mappings",
                                                            "Mapping evidence contains duplicate or invalid entries."));
            return Result<void>::Success();
        }

        void WriteAchievements(const AchievementDefinitionRegistry &registry, Writer &writer) {
            writer.U32(static_cast<std::uint32_t>(registry.Definitions().size()));
            for (const auto &definition : registry.Definitions()) {
                writer.U64(definition.id.value);
                writer.Byte(static_cast<std::uint8_t>(definition.authority));
                writer.Byte(static_cast<std::uint8_t>(definition.progress.kind));
                writer.U32(definition.progress.total);
                writer.Text(definition.presentation.titleLocalizationKey);
                writer.Text(definition.presentation.descriptionLocalizationKey);
                writer.Byte(definition.presentation.hidden);
            }
        }

        void WriteLeaderboards(const LeaderboardDefinitionRegistry &registry, Writer &writer) {
            writer.U32(static_cast<std::uint32_t>(registry.Definitions().size()));
            for (const auto &definition : registry.Definitions()) {
                writer.U64(definition.id.value);
                writer.Byte(static_cast<std::uint8_t>(definition.authority));
                writer.Byte(static_cast<std::uint8_t>(definition.valueKind));
                writer.U64(static_cast<std::uint64_t>(definition.range.minimum));
                writer.U64(static_cast<std::uint64_t>(definition.range.maximum));
                writer.Byte(static_cast<std::uint8_t>(definition.ordering));
                writer.Byte(definition.sourceStat.has_value());
                if (definition.sourceStat)
                    writer.U64(definition.sourceStat->value);
                writer.Text(definition.localizationKey);
                writer.Byte(definition.hidden);
            }
        }

        void WriteStats(const StatDefinitionRegistry &registry, Writer &writer) {
            writer.U32(static_cast<std::uint32_t>(registry.Definitions().size()));
            for (const auto &definition : registry.Definitions()) {
                writer.U64(definition.id.value);
                writer.Byte(static_cast<std::uint8_t>(definition.authority));
                writer.Byte(static_cast<std::uint8_t>(definition.valueKind));
                writer.U64(static_cast<std::uint64_t>(definition.range.minimum));
                writer.U64(static_cast<std::uint64_t>(definition.range.maximum));
                writer.Byte(static_cast<std::uint8_t>(definition.mutation));
                writer.Text(definition.localizationKey);
                writer.Byte(definition.hidden);
            }
        }

        void WritePresence(const PresenceDefinitionRegistry &registry, Writer &writer) {
            writer.U32(static_cast<std::uint32_t>(registry.Definitions().size()));
            for (const auto &definition : registry.Definitions()) {
                writer.U64(definition.id.value);
                writer.Byte(static_cast<std::uint8_t>(definition.detailPolicy));
                writer.U32(definition.maximumDetailUtf8Bytes);
                writer.Text(definition.localizationKey);
                writer.Byte(definition.hidden);
            }
        }

        void WriteNeutral(const PlatformProviderManifestCookInput &input, Writer &writer) {
            writer.Text("horo.platform-services.neutral.v1");
            writer.U32(PlatformProviderManifestCookSchemaVersion);
            writer.U32(PlatformStableIdRegistrySchemaVersion);
            writer.U32(AchievementDefinitionRegistrySchemaVersion);
            writer.U32(PlatformDefinitionRegistrySchemaVersion);
            writer.U32(PlatformProjectConfigurationSchemaVersion);
            writer.Text(input.stableIds.ProjectId());
            writer.Byte(static_cast<std::uint8_t>(input.configuration.Profile()));
            writer.Digest(input.stableIds.Fingerprint());
            writer.Digest(input.configuration.Fingerprint());
            writer.Digest(input.achievements.Fingerprint());
            writer.Digest(input.leaderboards.Fingerprint());
            writer.Digest(input.stats.Fingerprint());
            writer.Digest(input.presence.Fingerprint());
            for (const auto requirement : input.configuration.ServiceRequirements())
                writer.Byte(static_cast<std::uint8_t>(requirement));
            WriteAchievements(input.achievements, writer);
            WriteLeaderboards(input.leaderboards, writer);
            WriteStats(input.stats, writer);
            WritePresence(input.presence, writer);
        }

        void WriteMappings(const PlatformProviderManifestCookInput &input, const std::span<const PlatformProviderMappingEvidence> sorted,
                           Writer &writer) {
            writer.Text("horo.platform-services.mapping.v1");
            writer.U32(PlatformProviderManifestCookSchemaVersion);
            writer.Digest(input.stableIds.Fingerprint());
            writer.Digest(input.configuration.Fingerprint());
            writer.U64(input.configuration.SelectedProvider().value_or(PlatformProviderId{}).value);
            writer.U64(input.mappingRevision);
            for (const bool required : input.mappingPolicy.requiredKinds)
                writer.Byte(required);
            writer.U32(static_cast<std::uint32_t>(sorted.size()));
            for (const auto &mapping : sorted) {
                writer.Byte(static_cast<std::uint8_t>(mapping.kind));
                writer.U64(mapping.id.value);
                writer.Digest(mapping.providerValueDigest);
            }
        }
    }  // namespace

    namespace PlatformProviderManifestCookErrors {
        namespace {
            const ErrorDomainId Domain{"horo.platform.manifest-cook"};
        }

        const ErrorCodeDescriptor InvalidInput = MakeDescriptor(Domain, "platform.cook.invalid_input", "Cook input is invalid.",
                                                                "Supply one coherent validated project generation.", true);
        const ErrorCodeDescriptor StaleGeneration = MakeDescriptor(Domain, "platform.cook.stale_generation", "Cook generations disagree.",
                                                                   "Capture the registry, policy and mappings again.", true);
        const ErrorCodeDescriptor MissingMapping =
            MakeDescriptor(Domain, "platform.cook.mapping_missing", "Required provider mapping is missing.",
                           "Add a mapping for the diagnosed active identity.", true);
        const ErrorCodeDescriptor InvalidMapping = MakeDescriptor(Domain, "platform.cook.mapping_invalid", "Provider mapping is invalid.",
                                                                  "Correct the diagnosed mapping source.", true);
        const ErrorCodeDescriptor Cancelled =
            MakeDescriptor(Domain, "platform.cook.cancelled", "Cook was cancelled.", "Retry from a fresh captured generation.", false);
    }  // namespace PlatformProviderManifestCookErrors

    /** @copydoc CookPlatformProviderManifest */
    Result<PlatformProviderManifestCookOutput> CookPlatformProviderManifest(const PlatformProviderManifestCookInput &input) {
        using Output = PlatformProviderManifestCookOutput;
        if (IsCancelled(input))
            return Result<Output>::Failure(MakeError(PlatformProviderManifestCookErrors::Cancelled));
        if (!IsCoherent(input))
            return Result<Output>::Failure(MakeDiagnostic(PlatformProviderManifestCookErrors::StaleGeneration, "projectId",
                                                          "Policy or definitions target another stable-ID generation."));
        if (!RequiredPolicyIsCoherent(input))
            return Result<Output>::Failure(MakeDiagnostic(PlatformProviderManifestCookErrors::InvalidInput, "mappingPolicy",
                                                          "Required service kinds need mappings; disabled kinds cannot require them."));
        std::vector<PlatformProviderMappingEvidence> sorted;
        if (const auto validated = ValidateMappings(input, sorted); validated.HasError())
            return Result<Output>::Failure(validated.ErrorValue());
        if (IsCancelled(input))
            return Result<Output>::Failure(MakeError(PlatformProviderManifestCookErrors::Cancelled));
        Writer neutral;
        WriteNeutral(input, neutral);
        Writer mappings;
        WriteMappings(input, sorted, mappings);
        if (IsCancelled(input))
            return Result<Output>::Failure(MakeError(PlatformProviderManifestCookErrors::Cancelled));
        Output output{.neutralBytes = std::move(neutral).Take(), .mappingBytes = std::move(mappings).Take()};
        Writer binding;
        binding.Text("horo.platform-services.cook-generation.v1");
        binding.U32(static_cast<std::uint32_t>(output.neutralBytes.size()));
        binding.Bytes(output.neutralBytes);
        binding.U32(static_cast<std::uint32_t>(output.mappingBytes.size()));
        binding.Bytes(output.mappingBytes);
        const auto bindingBytes = std::move(binding).Take();
        output.generationFingerprint = ComputeSha256(bindingBytes);
        if (IsCancelled(input))
            return Result<Output>::Failure(MakeError(PlatformProviderManifestCookErrors::Cancelled));
        return Result<Output>::Success(std::move(output));
    }
}  // namespace Horo::PlatformServices
