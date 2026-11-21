#include "Horo/PlatformServices/PlatformProjectSettings.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::PlatformServices {
    namespace {
        /** @brief Builds the real contributed Foundation schema, without selecting or activating a backend. */
        ConfigurationSchema Schema() {
            ConfigurationSchema schema;
            const auto contribution = MakePlatformProjectSettingsContribution();
            for (const auto &setting : contribution.settings)
                REQUIRE(schema.Register(setting).HasValue());
            REQUIRE(schema.Seal().HasValue());
            return schema;
        }

        /** @brief Supplies a trusted inert provider eligible for all explicit host profiles. */
        PlatformProviderModuleContribution Contribution() {
            PlatformProviderModuleContribution provider{.module = ModuleId{"horo.platform.test"},
                                                        .providerKey = "platform.test",
                                                        .provider = PlatformProviderId{41},
                                                        .interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                             PlatformServicesBackendInterfaceMinor},
                                                        .allowedProfiles = PlatformServicesHostProfileMask::InteractiveDevelopment |
                                                                           PlatformServicesHostProfileMask::HeadlessServer |
                                                                           PlatformServicesHostProfileMask::Cook |
                                                                           PlatformServicesHostProfileMask::Certification};
            provider.supportedServices.fill(true);
            return provider;
        }
    }  // namespace

    TEST_CASE("Contributed project settings round-trip typed policy through the Foundation resolver",
              "[platform-services][project-settings]") {
        const auto provider = Contribution();
        const std::array providers{provider};
        const std::array trusted{provider.module};
        PlatformProjectConfigurationCandidate candidate{.projectId = "project.settings",
                                                        .profile = PlatformServicesHostProfile::Cook,
                                                        .provider = {.mode = PlatformProviderSelectionMode::ExactProvider,
                                                                     .providerKey = provider.providerKey}};
        candidate.services[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = PlatformServiceRequirement::Required;
        candidate.services[static_cast<std::size_t>(PlatformServiceKind::Cloud)] = PlatformServiceRequirement::Optional;
        const auto policy = BuildPlatformProjectConfiguration(candidate, providers, trusted);
        REQUIRE(policy.HasValue());
        const auto schema = Schema();
        const auto values = PlatformProjectSettingsValues(policy.Value());
        REQUIRE(values.HasValue());
        ConfigurationResolutionRequest request{.project = values.Value()};
        const auto snapshot = ConfigurationResolver::Resolve(schema, request, 17);
        REQUIRE(snapshot.HasValue());
        const auto projected = ResolvePlatformProjectSettings(snapshot.Value(), candidate.projectId, candidate.profile, providers, trusted);
        REQUIRE(projected.HasValue());
        CHECK(projected.Value().Fingerprint() == policy.Value().Fingerprint());
        CHECK(snapshot.Value().Revision() == 17);
        CHECK(snapshot.Value().FindResolved(SettingKey{"platform.services.achievements"})->source == ConfigurationSource::Project);
        REQUIRE_FALSE(schema.FindDescriptor(SettingKey{"platform.services.id_salt"}));
        REQUIRE_FALSE(schema.FindDescriptor(SettingKey{"platform.services.profile"}));
    }

    TEST_CASE("Portable platform settings cannot be overridden by ambient or private source layers",
              "[platform-services][project-settings]") {
        const auto schema = Schema();
        for (const auto source : {&ConfigurationResolutionRequest::invocation, &ConfigurationResolutionRequest::environment,
                                  &ConfigurationResolutionRequest::session, &ConfigurationResolutionRequest::user,
                                  &ConfigurationResolutionRequest::packagedProfile}) {
            ConfigurationResolutionRequest request;
            (request.*source)
                .emplace(SettingKey{"platform.services.provider.key"}, ConfigurationInputValue{.value = std::string{"platform.test"}});
            const auto snapshot = ConfigurationResolver::Resolve(schema, request);
            REQUIRE(snapshot.HasError());
            REQUIRE_FALSE(snapshot.ErrorValue().diagnostics.empty());
            CHECK(snapshot.ErrorValue().diagnostics.front().code.Value() == "configuration.source_forbidden");
        }
    }

    TEST_CASE("Portable descriptors declare shared validation and localized labels without changing source authority",
              "[platform-services][project-settings]") {
        const auto contribution = MakePlatformProjectSettingsContribution();
        REQUIRE(contribution.settings.size() == 8);
        for (const auto &descriptor : contribution.settings) {
            REQUIRE(descriptor.label);
            CHECK(descriptor.label->namespaceId == "editor");
            CHECK(descriptor.label->key.starts_with("settings.platform_services."));
            REQUIRE(descriptor.sourcePolicy);
            CHECK(descriptor.sourcePolicy->allowedSources == ConfigurationSourceMask::Project);
            CHECK(descriptor.reloadPolicy == ReloadPolicy::ProjectReopen);
            CHECK_FALSE(std::holds_alternative<std::monostate>(descriptor.validation));
        }
        const auto schema = Schema();
        for (const auto value : {std::int64_t{-1}, std::int64_t{2}}) {
            ConfigurationResolutionRequest request;
            request.project.emplace(SettingKey{"platform.services.provider.mode"}, ConfigurationInputValue{.value = value});
            const auto invalid = ConfigurationResolver::Resolve(schema, request);
            REQUIRE(invalid.HasError());
            REQUIRE_FALSE(invalid.ErrorValue().diagnostics.empty());
            CHECK(invalid.ErrorValue().diagnostics.front().path == "platform.services.provider.mode");
        }
        ConfigurationResolutionRequest request;
        request.project.emplace(SettingKey{"platform.services.provider.key"}, ConfigurationInputValue{.value = std::string(1025, 'x')});
        REQUIRE(ConfigurationResolver::Resolve(schema, request).HasError());
    }

    TEST_CASE("Contributed schema rejects invalid enum representations before projection", "[platform-services][project-settings]") {
        const auto schema = Schema();
        ConfigurationResolutionRequest request;
        SECTION("Unknown requirement representation") {
            request.project.emplace(SettingKey{"platform.services.achievements"}, ConfigurationInputValue{.value = std::int64_t{99}});
        }
        SECTION("Negative requirement representation") {
            request.project.emplace(SettingKey{"platform.services.achievements"}, ConfigurationInputValue{.value = std::int64_t{-1}});
        }
        const auto snapshot = ConfigurationResolver::Resolve(schema, request);
        REQUIRE(snapshot.HasError());
        REQUIRE_FALSE(snapshot.ErrorValue().diagnostics.empty());
        CHECK(snapshot.ErrorValue().diagnostics.front().path == "platform.services.achievements");
    }

    TEST_CASE("Contributed settings never turn explicit Null into a provider fallback", "[platform-services][project-settings]") {
        const auto uninitialized = PlatformProjectSettingsValues({});
        REQUIRE(uninitialized.HasError());
        CHECK(uninitialized.ErrorValue().code.Value() == PlatformProjectConfigurationErrors::InvalidConfiguration.code.Value());
        const auto schema = Schema();
        const auto defaults = ConfigurationResolver::Resolve(schema, {});
        REQUIRE(defaults.HasValue());
        const auto null =
            ResolvePlatformProjectSettings(defaults.Value(), "project.settings", PlatformServicesHostProfile::HeadlessServer, {}, {});
        REQUIRE(null.HasValue());
        CHECK(null.Value().UsesNullProvider());
        ConfigurationResolutionRequest request;
        request.project.emplace(SettingKey{"platform.services.achievements"}, ConfigurationInputValue{.value = std::int64_t{2}});
        const auto required = ConfigurationResolver::Resolve(schema, request);
        REQUIRE(required.HasValue());
        const auto invalid =
            ResolvePlatformProjectSettings(required.Value(), "project.settings", PlatformServicesHostProfile::Cook, {}, {});
        REQUIRE(invalid.HasError());
        CHECK(invalid.ErrorValue().code.Value() == PlatformProjectConfigurationErrors::RequiredCapabilityUnsupported.code.Value());
    }
}  // namespace Horo::PlatformServices
