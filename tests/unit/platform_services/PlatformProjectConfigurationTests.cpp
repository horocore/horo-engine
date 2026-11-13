#include "Horo/PlatformServices/PlatformProjectConfiguration.h"
#include "PlatformDefinitionTestAssertions.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace Horo::PlatformServices {
    using TestAssertions::CheckError;

    namespace {
        constexpr auto AllProfiles = PlatformServicesHostProfileMask::InteractiveDevelopment |
                                     PlatformServicesHostProfileMask::HeadlessServer | PlatformServicesHostProfileMask::Cook |
                                     PlatformServicesHostProfileMask::Certification;

        [[nodiscard]] PlatformProviderModuleContribution Contribution(std::string module = "horo.platform.steam",
                                                                      std::string key = "platform.provider.steam",
                                                                      const std::uint64_t id = 41) {
            PlatformProviderModuleContribution contribution{
                .module = {std::move(module)},
                .providerKey = std::move(key),
                .provider = {id},
                .interfaceVersion = {PlatformServicesBackendInterfaceMajor, PlatformServicesBackendInterfaceMinor},
                .allowedProfiles = AllProfiles,
            };
            contribution.supportedServices.fill(true);
            return contribution;
        }

        [[nodiscard]] PlatformProjectConfigurationCandidate Candidate(std::string providerKey = "platform.provider.steam") {
            PlatformProjectConfigurationCandidate candidate{
                .projectId = "project.platform-services",
                .profile = PlatformServicesHostProfile::InteractiveDevelopment,
                .provider = {.mode = PlatformProviderSelectionMode::ExactProvider, .providerKey = std::move(providerKey)},
            };
            candidate.services.fill(PlatformServiceRequirement::Optional);
            candidate.services[static_cast<std::size_t>(PlatformServiceKind::Session)] = PlatformServiceRequirement::Required;
            return candidate;
        }

        [[nodiscard]] Result<PlatformProjectConfiguration> Build(const PlatformProjectConfigurationCandidate &candidate,
                                                                 std::vector<PlatformProviderModuleContribution> contributions = {
                                                                     Contribution()}) {
            std::vector<ModuleId> trusted;
            trusted.reserve(contributions.size());
            for (const auto &contribution : contributions)
                trusted.push_back(contribution.module);
            return BuildPlatformProjectConfiguration(candidate, contributions, trusted);
        }
    }  // namespace

    TEST_CASE("Platform project policy is typed deterministic and backend neutral", "[platform-services][project-configuration]") {
        auto candidate = Candidate();
        const auto firstContribution = Contribution();
        const auto secondContribution = Contribution("horo.platform.mock", "platform.provider.mock", 72);
        const std::vector firstOrder{firstContribution, secondContribution};
        const std::vector reverseOrder{secondContribution, firstContribution};
        const std::vector trusted{firstContribution.module, secondContribution.module};

        const auto first = BuildPlatformProjectConfiguration(candidate, firstOrder, trusted);
        const auto second = BuildPlatformProjectConfiguration(candidate, reverseOrder, trusted);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        CHECK(first.Value().ProjectId() == "project.platform-services");
        CHECK(first.Value().Profile() == PlatformServicesHostProfile::InteractiveDevelopment);
        CHECK_FALSE(first.Value().UsesNullProvider());
        CHECK(first.Value().SelectedProvider() == PlatformProviderId{41});
        CHECK(first.Value().SelectedProviderKey() == "platform.provider.steam");
        REQUIRE(first.Value().SelectedModule().has_value());
        CHECK(first.Value().SelectedModule()->value == "horo.platform.steam");
        CHECK(first.Value().Fingerprint() == second.Value().Fingerprint());
        CHECK(first.Value().BackendConfig().requiredServices[static_cast<std::size_t>(PlatformServiceKind::Session)]);
        CHECK_FALSE(first.Value().BackendConfig().requiredServices[static_cast<std::size_t>(PlatformServiceKind::Cloud)]);
    }

    TEST_CASE("Explicit Null is a safe default and never satisfies required service policy", "[platform-services][project-configuration]") {
        PlatformProjectConfigurationCandidate candidate{.projectId = "project.headless",
                                                        .profile = PlatformServicesHostProfile::HeadlessServer};
        const auto nullPolicy = BuildPlatformProjectConfiguration(candidate, {}, {});
        REQUIRE(nullPolicy.HasValue());
        CHECK(nullPolicy.Value().UsesNullProvider());
        CHECK_FALSE(nullPolicy.Value().SelectedProvider());
        CHECK_FALSE(nullPolicy.Value().SelectedModule());

        candidate.services[static_cast<std::size_t>(PlatformServiceKind::Session)] = PlatformServiceRequirement::Required;
        CheckError(BuildPlatformProjectConfiguration(candidate, {}, {}), PlatformProjectConfigurationErrors::RequiredCapabilityUnsupported);
    }

    TEST_CASE("Exact provider selection never falls back to another trusted contribution", "[platform-services][project-configuration]") {
        const auto available = Contribution("horo.platform.mock", "platform.provider.mock", 72);
        const std::vector contributions{available};
        const std::vector trusted{available.module};
        CheckError(BuildPlatformProjectConfiguration(Candidate(), contributions, trusted),
                   PlatformProjectConfigurationErrors::ProviderNotFound);
    }

    TEST_CASE("Required capabilities and profile gates fail before activation", "[platform-services][project-configuration]") {
        auto contribution = Contribution();
        contribution.supportedServices[static_cast<std::size_t>(PlatformServiceKind::Session)] = false;
        CheckError(Build(Candidate(), {contribution}), PlatformProjectConfigurationErrors::RequiredCapabilityUnsupported);

        contribution = Contribution();
        contribution.allowedProfiles = PlatformServicesHostProfileMask::InteractiveDevelopment;
        auto certification = Candidate();
        certification.profile = PlatformServicesHostProfile::Certification;
        CheckError(Build(certification, {contribution}), PlatformProjectConfigurationErrors::ProfileDenied);
    }

    TEST_CASE("Only explicitly trusted inert module contributions enter policy", "[platform-services][project-configuration]") {
        const auto contribution = Contribution();
        CheckError(BuildPlatformProjectConfiguration(Candidate(), std::span{&contribution, 1}, {}),
                   PlatformProjectConfigurationErrors::UntrustedContribution);

        auto duplicate = Contribution("horo.platform.other", "platform.provider.steam", 99);
        const std::vector contributions{contribution, duplicate};
        const std::vector trusted{contribution.module, duplicate.module};
        CheckError(BuildPlatformProjectConfiguration(Candidate(), contributions, trusted),
                   PlatformProjectConfigurationErrors::DuplicateContribution);
    }

    TEST_CASE("Malformed version identities enums and bounds fail with stable diagnostics", "[platform-services][project-configuration]") {
        auto candidate = Candidate();
        ++candidate.schemaVersion;
        CheckError(Build(candidate), PlatformProjectConfigurationErrors::UnsupportedVersion);
        candidate = Candidate();
        candidate.projectId.clear();
        CheckError(Build(candidate), PlatformProjectConfigurationErrors::InvalidConfiguration);
        candidate = Candidate();
        candidate.projectId.push_back(static_cast<char>(0xff));
        CheckError(Build(candidate), PlatformProjectConfigurationErrors::InvalidConfiguration);
        candidate = Candidate();
        candidate.services.front() = static_cast<PlatformServiceRequirement>(99);
        CheckError(Build(candidate), PlatformProjectConfigurationErrors::InvalidConfiguration);
        candidate = Candidate();
        const std::vector contributions{Contribution()};
        const std::vector trusted{ModuleId{"horo.platform.steam"}};
        CheckError(BuildPlatformProjectConfiguration(candidate, contributions, trusted, {.maximumContributions = 0}),
                   PlatformProjectConfigurationErrors::CapacityExceeded);
    }

    TEST_CASE("Contribution validation rejects ABI drift native-like invalid identities and conflicting owners",
              "[platform-services][project-configuration]") {
        auto contribution = Contribution();
        ++contribution.interfaceVersion.minor;
        CheckError(Build(Candidate(), {contribution}), PlatformProjectConfigurationErrors::InvalidContribution);
        contribution = Contribution();
        contribution.provider = {};
        CheckError(Build(Candidate(), {contribution}), PlatformProjectConfigurationErrors::InvalidContribution);
        contribution = Contribution();
        contribution.module.value = "SteamNativeHandle*";
        CheckError(Build(Candidate(), {contribution}), PlatformProjectConfigurationErrors::InvalidContribution);
    }

    TEST_CASE("Replacement is fingerprint fenced and failure preserves the prior immutable policy",
              "[platform-services][project-configuration]") {
        const auto contribution = Contribution();
        const std::vector contributions{contribution};
        const std::vector trusted{contribution.module};
        const auto initial = BuildPlatformProjectConfiguration(Candidate(), contributions, trusted);
        REQUIRE(initial.HasValue());
        const Sha256Digest originalFingerprint = initial.Value().Fingerprint();

        auto replacement = Candidate();
        replacement.baseFingerprint = originalFingerprint;
        replacement.services[static_cast<std::size_t>(PlatformServiceKind::Presence)] = PlatformServiceRequirement::Disabled;
        const auto updated = BuildPlatformProjectConfigurationReplacement(initial.Value(), replacement, contributions, trusted);
        REQUIRE(updated.HasValue());
        CHECK(updated.Value().Fingerprint() != originalFingerprint);

        replacement.baseFingerprint->bytes.front() ^= 0xffU;
        CheckError(BuildPlatformProjectConfigurationReplacement(initial.Value(), replacement, contributions, trusted),
                   PlatformProjectConfigurationErrors::StaleReplacement);
        CHECK(initial.Value().Fingerprint() == originalFingerprint);
        CheckError(BuildPlatformProjectConfiguration(replacement, contributions, trusted),
                   PlatformProjectConfigurationErrors::StaleReplacement);
    }
}  // namespace Horo::PlatformServices
