#include "../../support/project/PlatformServicesAuthoringFixture.h"

namespace Horo::Application {
    using namespace PlatformServices;
    using namespace Horo::Tests::PlatformAuthoring;

    TEST_CASE("Canonical project capture resolves portable policy for every host profile", "[platform-authoring]") {
        const auto root = Root();
        const auto capture = DecodeProjectSourceDocument(root.dump());
        REQUIRE(capture.HasValue());
        const auto salt = ResolveProjectPlatformServicesSalt(capture.Value());
        REQUIRE(salt.HasValue());
        CHECK(salt.Value() == Candidate().salt);
        for (const auto profile : {PlatformServicesHostProfile::InteractiveDevelopment, PlatformServicesHostProfile::HeadlessServer,
                                   PlatformServicesHostProfile::Cook, PlatformServicesHostProfile::Certification}) {
            const auto configuration = ResolveProjectPlatformServicesConfiguration(capture.Value(), profile, {}, {});
            REQUIRE(configuration.HasValue());
            CHECK(configuration.Value().UsesNullProvider());
            CHECK(configuration.Value().Profile() == profile);
            CHECK(configuration.Value().ProjectId() == capture.Value().metadata.projectId);
            CHECK(nlohmann::json::parse(EncodePolicy(configuration.Value())) == root["settings"]["platformServices"]);
        }
    }

    TEST_CASE("Policy edit preserves project identity salt and unrelated authored values", "[platform-authoring][project-settings]") {
        auto root = Root();
        root["settings"]["unrelated"] = {{"nested", {1, "Türkçe", false}}};
        root["customRoot"] = "retained";
        const auto original = root.dump();
        PlatformProjectConfigurationCandidate candidate{.projectId = "project.portable"};
        candidate.services[static_cast<std::size_t>(PlatformServiceKind::Cloud)] = PlatformServiceRequirement::Optional;
        const auto policy = BuildPlatformProjectConfiguration(candidate, {}, {});
        REQUIRE(policy.HasValue());
        const auto edited = PreparePlatformServicesProjectPolicyEdit(original, policy.Value());
        REQUIRE(edited.HasValue());
        auto expected = root;
        expected["settings"]["platformServices"] = nlohmann::json::parse(EncodePolicy(policy.Value()));
        CHECK(nlohmann::json::parse(edited.Value()) == expected);
        CHECK(root.dump() == original);
        CHECK(edited.Value().back() == '\n');
        const auto second = PreparePlatformServicesProjectPolicyEdit(edited.Value(), policy.Value());
        REQUIRE(second.HasValue());
        CHECK(second.Value() == edited.Value());
        const auto decoded = DecodeProjectSourceDocument(edited.Value());
        REQUIRE(decoded.HasValue());
        const auto resolved = ResolveProjectPlatformServicesConfiguration(decoded.Value(), candidate.profile, {}, {});
        REQUIRE(resolved.HasValue());
        CHECK(resolved.Value().Fingerprint() == policy.Value().Fingerprint());
    }

    TEST_CASE("Policy edit refuses partial legacy foreign or uninitialized authority", "[platform-authoring][project-settings]") {
        auto root = Root();
        const auto policy = BuildPlatformProjectConfiguration({.projectId = "project.portable"}, {}, {});
        REQUIRE(policy.HasValue());
        SECTION("Missing salt") {
            root.erase("platformServicesIdSalt");
        }
        SECTION("Missing policy") {
            root["settings"].erase("platformServices");
        }
        SECTION("Malformed salt") {
            root["platformServicesIdSalt"] = "invalid";
        }
        SECTION("Foreign project") {
            root["projectId"] = "project.foreign";
        }
        SECTION("Unknown policy member") {
            root["settings"]["platformServices"]["unknown"] = true;
        }
        SECTION("Uninitialized candidate") {
            REQUIRE(PreparePlatformServicesProjectPolicyEdit(root.dump(), {}).HasError());
            return;
        }
        REQUIRE(PreparePlatformServicesProjectPolicyEdit(root.dump(), policy.Value()).HasError());
    }

    TEST_CASE("Root salt absence and malformed authority never trigger read-only repair", "[platform-authoring]") {
        auto root = Root();
        SECTION("Missing salt") {
            root.erase("platformServicesIdSalt");
        }
        SECTION("Zero salt") {
            root["platformServicesIdSalt"] = "psid1:00000000000000000000000000000000";
        }
        SECTION("Numeric salt") {
            root["platformServicesIdSalt"] = 1;
        }
        SECTION("Wrong type") {
            root["platformServicesIdSalt"] = nlohmann::json::object();
        }
        SECTION("Noncanonical salt") {
            root["platformServicesIdSalt"] = "psid1:0102030405060708090A0b0c0d0e0f10";
        }
        const auto original = root.dump();
        const auto captured = DecodeProjectSourceDocument(original);
        REQUIRE(captured.HasValue());
        const auto diagnostic = ResolveProjectPlatformServicesSalt(captured.Value());
        CheckError(diagnostic, StableIdErrors::InvalidSalt);
        REQUIRE_FALSE(diagnostic.ErrorValue().diagnostics.empty());
        CHECK(diagnostic.ErrorValue().diagnostics.front().location.source == ".horo/project.json");
        CHECK(diagnostic.ErrorValue().diagnostics.front().path == "platformServicesIdSalt");
        CheckError(ResolveProjectPlatformServicesConfiguration(captured.Value(), PlatformServicesHostProfile::Cook, {}, {}),
                   StableIdErrors::InvalidSalt);
        CHECK(root.dump() == original);
    }

    TEST_CASE("All host profiles reject the same malformed portable project policy", "[platform-authoring]") {
        auto root = Root();
        auto &policy = root["settings"]["platformServices"];
        const ErrorCodeDescriptor *expected = &PlatformProjectConfigurationErrors::InvalidConfiguration;
        SECTION("Missing policy") {
            root["settings"].erase("platformServices");
        }
        SECTION("Unsupported schema") {
            policy["schemaVersion"] = 2;
            expected = &PlatformProjectConfigurationErrors::UnsupportedVersion;
        }
        SECTION("Deprecated field") {
            policy["steamAppId"] = 480;
        }
        SECTION("Host profile override") {
            policy["profile"] = "certification";
        }
        SECTION("Wrong service count") {
            policy["services"] = {0, 0};
        }
        SECTION("Unknown service requirement") {
            policy["services"][0] = 3;
        }
        SECTION("Fractional requirement") {
            policy["services"][0] = 1.0;
        }
        SECTION("Negative requirement") {
            policy["services"][0] = -1;
        }
        SECTION("Provider fallback request") {
            policy["provider"]["fallback"] = "null";
        }
        SECTION("Unknown provider selection") {
            policy["provider"]["mode"] = "auto";
        }
        const auto captured = DecodeProjectSourceDocument(root.dump());
        REQUIRE(captured.HasValue());
        for (const auto profile : {PlatformServicesHostProfile::InteractiveDevelopment, PlatformServicesHostProfile::HeadlessServer,
                                   PlatformServicesHostProfile::Cook, PlatformServicesHostProfile::Certification})
            CheckError(ResolveProjectPlatformServicesConfiguration(captured.Value(), profile, {}, {}), *expected);
    }

    TEST_CASE("Project policy codec honors the common validator's explicit identity limits", "[platform-authoring]") {
        auto root = Root();
        const std::string key = "platform." + std::string(192, 'a');
        root["settings"]["platformServices"]["provider"] = {{"mode", "exact"}, {"key", key}};
        const auto captured = DecodeProjectSourceDocument(root.dump());
        REQUIRE(captured.HasValue());
        PlatformProviderModuleContribution contribution{.module = ModuleId{"horo.platform.test"},
                                                        .providerKey = key,
                                                        .provider = PlatformProviderId{41},
                                                        .interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                             PlatformServicesBackendInterfaceMinor},
                                                        .allowedProfiles = PlatformServicesHostProfileMask::Cook};
        contribution.supportedServices.fill(true);
        const std::array contributions{contribution};
        const std::array trusted{contribution.module};
        const auto policy = ResolveProjectPlatformServicesConfiguration(captured.Value(), PlatformServicesHostProfile::Cook, contributions,
                                                                        trusted, {.maximumProviderKeyBytes = 256});
        REQUIRE(policy.HasValue());
        CHECK(policy.Value().SelectedProviderKey() == key);
        CheckError(ResolveProjectPlatformServicesConfiguration(captured.Value(), PlatformServicesHostProfile::Cook, contributions, trusted),
                   PlatformProjectConfigurationErrors::InvalidConfiguration);
    }

    TEST_CASE("Project policy preserves exact provider and required-capability failures", "[platform-authoring]") {
        auto root = Root();
        root["settings"]["platformServices"]["services"][0] = 2;
        auto captured = DecodeProjectSourceDocument(root.dump());
        REQUIRE(captured.HasValue());
        CheckError(ResolveProjectPlatformServicesConfiguration(captured.Value(), PlatformServicesHostProfile::Cook, {}, {}),
                   PlatformProjectConfigurationErrors::RequiredCapabilityUnsupported);
        root["settings"]["platformServices"]["provider"] = {{"mode", "exact"}, {"key", "platform.missing"}};
        captured = DecodeProjectSourceDocument(root.dump());
        REQUIRE(captured.HasValue());
        CheckError(ResolveProjectPlatformServicesConfiguration(captured.Value(), PlatformServicesHostProfile::Cook, {}, {}),
                   PlatformProjectConfigurationErrors::ProviderNotFound);
        CheckError(ResolveProjectPlatformServicesConfiguration(captured.Value(), PlatformServicesHostProfile::Cook, {}, {},
                                                               {.maximumProviderKeyBytes = 0}),
                   PlatformProjectConfigurationErrors::CapacityExceeded);
        auto duplicate = root.dump();
        duplicate.insert(duplicate.find("\"platformServicesIdSalt\""), "\"platformServicesIdSalt\":\"bad\",");
        REQUIRE(DecodeProjectSourceDocument(duplicate).HasError());
    }

}  // namespace Horo::Application
