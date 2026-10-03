#include "Horo/Network/NetworkTargetCapabilities.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>

namespace {
    using namespace Horo::Network;

    NetworkProjectSettings MakeProject() {
        auto input = DefaultNetworkProjectSettings(NetworkProjectSettingsId::Create(10).Value()).Value();
        input.supportedRoles = NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::Client | NetworkProjectRoleSet::ListenServer |
                               NetworkProjectRoleSet::DedicatedServer;
        input.transport.requirement = NetworkProjectTransportRequirement::Required;
        input.transport.capabilities.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
        input.transport.capabilities.requiredChannels = 2;
        input.transport.capabilities.requiredMaximumMessageBytes = 1024;
        return NetworkProjectSettings::Create(input).Value();
    }

    TransportCapabilities Capabilities() {
        TransportCapabilities capabilities;
        capabilities.revision = 1;
        capabilities.delivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = TransportSupport::Available;
        capabilities.maximumChannels = 4;
        capabilities.maximumMessageBytes = 4096;
        return capabilities;
    }

    struct Fixture final {
        NetworkProjectSettings project = MakeProject();
        NetworkProductCapabilityManifest product{};
        NetworkTargetPackageInventory inventory{};
        NetworkTargetHostFacts host{};
        NetworkTargetRequirements requirements{};
        NetworkTargetSelection selection{};

        Fixture() {
            const auto provider = NetworkTransportProviderId::Create(7).Value();
            product.build = NetworkProductBuildId::Create(100).Value();
            product.revision = NetworkProductCapabilityRevision::Create(1).Value();
            product.platform = NetworkTargetPlatform::Linux;
            product.supportedRoles = project.SupportedRoles();
            product.includesNetworkRuntime = true;
            product.profile = project.Profile().id;
            product.profileRevision = project.Profile().revision;
            product.protocol = project.Protocol();
            product.providerCount = 1;
            product.providers[0] = {provider, std::uint32_t{1} << static_cast<std::uint8_t>(NetworkTargetPlatform::Linux), Capabilities()};
            inventory.build = product.build;
            inventory.platform = product.platform;
            inventory.supportedRoles = product.supportedRoles;
            inventory.includesNetworkRuntime = true;
            inventory.protocol = product.protocol;
            inventory.providerCount = 1;
            inventory.providers[0] = product.providers[0];
            host.revision = NetworkHostCapabilityRevision::Create(2).Value();
            host.platform = product.platform;
            host.supportedRoles = product.supportedRoles;
            host.networkRuntimeInstalled = true;
            host.protocol = product.protocol;
            host.providerCount = 1;
            host.providers[0] = {provider, true, true, true, Capabilities()};
            requirements.requiredRoles = NetworkProjectRoleSet::Client;
            requirements.requiredProvider = provider;
            selection.role = NetworkProjectRole::Client;
            selection.provider = provider;
            selection.protocolVersion = {.major = 1, .minor = 0};
            selection.expectedBuild = product.build;
            selection.expectedProductRevision = product.revision;
            selection.expectedHostRevision = host.revision;
            selection.expectedProjectRevision = project.Revision();
        }

        NetworkTargetAssessment Assess() const {
            return AssessNetworkTarget(project, product, inventory, host, requirements, selection);
        }
    };

    TEST_CASE("Network target admits explicit client listen and dedicated roles without renderer tier", "[unit][network][target]") {
        Fixture fixture;
        for (const auto role : {NetworkProjectRole::Client, NetworkProjectRole::ListenServer, NetworkProjectRole::DedicatedServer}) {
            fixture.selection.role = role;
            const auto assessment = fixture.Assess();
            REQUIRE(assessment.Admitted());
            REQUIRE(assessment.matrix.roles[static_cast<std::size_t>(role)].packaged);
            REQUIRE(assessment.matrix.roles[static_cast<std::size_t>(role)].hostSupported);
            REQUIRE(assessment.matrix.roles[static_cast<std::size_t>(role)].selected);
            REQUIRE(assessment.matrix.providers[0].packaged);
            REQUIRE(assessment.matrix.providers[0].installed);
            REQUIRE(assessment.matrix.providers[0].hostSupported);
            REQUIRE(assessment.matrix.providers[0].configured);
            REQUIRE(assessment.matrix.providers[0].projectRequired);
            REQUIRE(assessment.matrix.providers[0].selected);
            REQUIRE(assessment.matrix.protocolPackaged);
            REQUIRE(assessment.matrix.protocolHostSupported);
            REQUIRE(assessment.matrix.protocolProjectRequired);
            REQUIRE(assessment.matrix.protocolSelected);
        }
        fixture.selection.role = NetworkProjectRole::Standalone;
        fixture.selection.provider = {};
        fixture.selection.protocolVersion = {};
        REQUIRE(fixture.Assess().Admitted());
    }

    TEST_CASE("Standalone package requires neither runtime nor provider and cannot claim client", "[unit][network][target]") {
        Fixture fixture;
        fixture.product.supportedRoles = NetworkProjectRoleSet::Standalone;
        fixture.product.includesNetworkRuntime = false;
        fixture.product.protocol = {};
        fixture.product.providerCount = 0;
        fixture.inventory.supportedRoles = NetworkProjectRoleSet::Standalone;
        fixture.inventory.includesNetworkRuntime = false;
        fixture.inventory.protocol = {};
        fixture.inventory.providerCount = 0;
        fixture.host.supportedRoles = NetworkProjectRoleSet::Standalone;
        fixture.host.networkRuntimeInstalled = false;
        fixture.host.protocol = {};
        fixture.host.providerCount = 0;
        fixture.requirements.requiredRoles = NetworkProjectRoleSet::Standalone;
        fixture.requirements.requiredProvider = {};
        fixture.selection.role = NetworkProjectRole::Standalone;
        fixture.selection.provider = {};
        fixture.selection.protocolVersion = {};
        REQUIRE(fixture.Assess().Admitted());
        fixture.selection.role = NetworkProjectRole::Client;
        fixture.selection.provider = NetworkTransportProviderId::Create(7).Value();
        fixture.selection.protocolVersion = {.major = 1, .minor = 0};
        REQUIRE(fixture.Assess().diagnostic.reason == NetworkTargetFailureReason::NotPackaged);
    }

    TEST_CASE("Network target reports missing project product and host roles", "[unit][network][target]") {
        Fixture fixture;
        fixture.product.supportedRoles =
            NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::ListenServer | NetworkProjectRoleSet::DedicatedServer;
        fixture.inventory.supportedRoles = fixture.product.supportedRoles;
        auto assessment = fixture.Assess();
        REQUIRE_FALSE(assessment.Admitted());
        REQUIRE(assessment.diagnostic.capability == NetworkTargetCapabilityKind::Role);
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::NotPackaged);
        REQUIRE(assessment.diagnostic.remediation == NetworkTargetRemediation::RebuildPackage);
        REQUIRE(assessment.diagnostic.role == NetworkProjectRole::Client);

        fixture = Fixture{};
        fixture.host.supportedRoles =
            NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::ListenServer | NetworkProjectRoleSet::DedicatedServer;
        assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::HostUnsupported);
        REQUIRE(assessment.diagnostic.role == NetworkProjectRole::Client);

        fixture = Fixture{};
        fixture.requirements.requiredRoles = static_cast<NetworkProjectRoleSet>(1U << 5U);
        assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.capability == NetworkTargetCapabilityKind::Input);
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::Invalid);
    }

    TEST_CASE("Network target rejects package role protocol and provider inventory mismatch", "[unit][network][target]") {
        Fixture fixture;
        fixture.inventory.supportedRoles = NetworkProjectRoleSet::Standalone;
        auto assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.capability == NetworkTargetCapabilityKind::Role);
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::PackageMismatch);
        REQUIRE(assessment.diagnostic.role == NetworkProjectRole::Client);

        fixture = Fixture{};
        fixture.inventory.protocol.schemaFingerprint = 10;
        assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.capability == NetworkTargetCapabilityKind::Protocol);
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::PackageMismatch);
        REQUIRE(assessment.diagnostic.protocol == fixture.project.Protocol().protocol);
        REQUIRE_FALSE(assessment.matrix.protocolPackaged);

        fixture = Fixture{};
        fixture.inventory.providerCount = 0;
        assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::PackageMismatch);
        REQUIRE(assessment.diagnostic.provider == fixture.selection.provider);
        REQUIRE(assessment.diagnostic.remediation == NetworkTargetRemediation::RebuildPackage);

        fixture = Fixture{};
        fixture.inventory.providers[0].capabilities.maximumChannels = 3;
        assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.capability == NetworkTargetCapabilityKind::Provider);
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::PackageMismatch);
        REQUIRE(assessment.diagnostic.provider == fixture.selection.provider);
    }

    TEST_CASE("Network target reports installed supported configured and runtime host states", "[unit][network][target]") {
        Fixture fixture;
        fixture.host.providers[0].installed = false;
        fixture.host.providers[0].hostSupported = false;
        fixture.host.providers[0].configured = false;
        auto assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::NotInstalled);
        REQUIRE(assessment.diagnostic.provider == fixture.selection.provider);
        REQUIRE(assessment.diagnostic.remediation == NetworkTargetRemediation::InstallTarget);

        fixture = Fixture{};
        fixture.host.providers[0].hostSupported = false;
        assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::HostUnsupported);
        REQUIRE(assessment.diagnostic.remediation == NetworkTargetRemediation::ChooseSupportedHost);

        fixture = Fixture{};
        fixture.host.providers[0].configured = false;
        assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::ProjectUnavailable);
        REQUIRE(assessment.diagnostic.remediation == NetworkTargetRemediation::ConfigureProject);

        fixture = Fixture{};
        fixture.host.networkRuntimeInstalled = false;
        assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.capability == NetworkTargetCapabilityKind::NetworkRuntime);
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::NotInstalled);
    }

    TEST_CASE("Network target requires exact packaged and selected provider", "[unit][network][target]") {
        Fixture fixture;
        fixture.requirements.requiredProvider = NetworkTransportProviderId::Create(8).Value();
        auto assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::NotPackaged);
        REQUIRE(assessment.diagnostic.provider == fixture.requirements.requiredProvider);

        fixture.product.providerCount = 2;
        fixture.product.providers[1] = fixture.product.providers[0];
        fixture.product.providers[1].id = fixture.requirements.requiredProvider;
        fixture.inventory.providerCount = 2;
        fixture.inventory.providers[1] = fixture.product.providers[1];
        fixture.host.providerCount = 2;
        fixture.host.providers[1] = fixture.host.providers[0];
        fixture.host.providers[1].id = fixture.requirements.requiredProvider;
        assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::NotSelected);
        REQUIRE(assessment.diagnostic.provider == fixture.requirements.requiredProvider);
    }

    TEST_CASE("Network target rejects platform protocol and transport mismatch without fallback", "[unit][network][target]") {
        Fixture fixture;
        fixture.host.platform = NetworkTargetPlatform::Windows;
        auto assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.capability == NetworkTargetCapabilityKind::Platform);
        REQUIRE(assessment.diagnostic.platform == NetworkTargetPlatform::Linux);

        fixture = Fixture{};
        fixture.product.providers[0].allowedPlatforms = std::uint32_t{1} << static_cast<std::uint8_t>(NetworkTargetPlatform::Windows);
        fixture.inventory.providers[0].allowedPlatforms = fixture.product.providers[0].allowedPlatforms;
        assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.capability == NetworkTargetCapabilityKind::Platform);
        REQUIRE(assessment.diagnostic.provider == fixture.selection.provider);

        fixture = Fixture{};
        fixture.host.protocol.schemaFingerprint = 2;
        assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.capability == NetworkTargetCapabilityKind::Protocol);
        REQUIRE(assessment.diagnostic.protocol == fixture.project.Protocol().protocol);
        REQUIRE(assessment.diagnostic.remediation == NetworkTargetRemediation::UpgradeProtocol);

        fixture = Fixture{};
        fixture.host.providers[0].capabilities.maximumChannels = 1;
        assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.capability == NetworkTargetCapabilityKind::Provider);
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::Incompatible);
        REQUIRE(assessment.diagnostic.remediation == NetworkTargetRemediation::SelectAvailableCapability);

        fixture = Fixture{};
        fixture.inventory.providers[0].id = NetworkTransportProviderId::Create(9).Value();
        assessment = fixture.Assess();
        REQUIRE(assessment.diagnostic.reason == NetworkTargetFailureReason::PackageMismatch);
    }

    TEST_CASE("Network target fences stale replacement cancellation and shutdown", "[unit][network][target]") {
        Fixture fixture;
        fixture.selection.expectedBuild = NetworkProductBuildId::Create(99).Value();
        REQUIRE(fixture.Assess().diagnostic.reason == NetworkTargetFailureReason::Stale);
        fixture = Fixture{};
        fixture.selection.expectedHostRevision = NetworkHostCapabilityRevision::Create(1).Value();
        REQUIRE(fixture.Assess().diagnostic.reason == NetworkTargetFailureReason::Stale);
        fixture = Fixture{};
        fixture.selection.lifecycle = NetworkTargetLifecycle::Cancelling;
        REQUIRE(fixture.Assess().diagnostic.reason == NetworkTargetFailureReason::Cancelled);
        fixture.selection.lifecycle = NetworkTargetLifecycle::ShuttingDown;
        REQUIRE(fixture.Assess().diagnostic.reason == NetworkTargetFailureReason::ShuttingDown);
        fixture = Fixture{};
        fixture.product.profileRevision = NetworkProjectProfileRevision::Create(2).Value();
        REQUIRE(fixture.Assess().diagnostic.capability == NetworkTargetCapabilityKind::ProjectProfile);
    }

    TEST_CASE("Product manifest is closed bounded and deterministic for packaging automation", "[unit][network][target]") {
        Fixture fixture;
        const auto encoded = SerializeNetworkProductCapabilityManifest(fixture.product);
        REQUIRE(encoded.HasValue());
        const auto parsed = ParseNetworkProductCapabilityManifest(encoded.Value());
        REQUIRE(parsed.HasValue());
        REQUIRE(SerializeNetworkProductCapabilityManifest(parsed.Value()).Value() == encoded.Value());
        REQUIRE(
            AssessNetworkTarget(fixture.project, parsed.Value(), fixture.inventory, fixture.host, fixture.requirements, fixture.selection)
                .Admitted());
        auto document = nlohmann::json::parse(encoded.Value());
        document["credentialReference"] = "secret";
        const auto rejectedSecret = ParseNetworkProductCapabilityManifest(document.dump());
        REQUIRE(rejectedSecret.HasError());
        REQUIRE(rejectedSecret.ErrorValue().code.Value() == "network.target_manifest.invalid");
        document = nlohmann::json::parse(encoded.Value());
        document["contractVersion"] = 2;
        REQUIRE(ParseNetworkProductCapabilityManifest(document.dump()).HasError());
        REQUIRE(ParseNetworkProductCapabilityManifest("{\"build\":1,\"build\":2}").HasError());
        const auto oversized = ParseNetworkProductCapabilityManifest(std::string(16 * 1024 + 1, 'x'));
        REQUIRE(oversized.HasError());
        REQUIRE(oversized.ErrorValue().code.Value() == "network.target_manifest.capacity_exceeded");
        document = nlohmann::json::parse(encoded.Value());
        document["providers"][0]["id"] = document["providers"][0]["id"].get<std::uint64_t>() + 1;
        REQUIRE(ParseNetworkProductCapabilityManifest(document.dump()).HasValue());
    }

    TEST_CASE("Migrated project settings still require the exact packaged target", "[unit][network][target]") {
        Fixture fixture;
        auto legacy = nlohmann::json::parse(SerializeNetworkProjectSettings(fixture.project));
        legacy["contractVersion"] = 1;
        legacy.erase("defaultEndpoint");
        legacy.erase("credentialRequirementId");
        const auto migrated = ParseNetworkProjectSettings(legacy.dump());
        REQUIRE(migrated.HasValue());
        const auto migratedProject = NetworkProjectSettings::Create(migrated.Value());
        REQUIRE(migratedProject.HasValue());
        REQUIRE(AssessNetworkTarget(migratedProject.Value(), fixture.product, fixture.inventory, fixture.host, fixture.requirements,
                                    fixture.selection)
                    .Admitted());
        fixture.inventory.providerCount = 0;
        REQUIRE(AssessNetworkTarget(migratedProject.Value(), fixture.product, fixture.inventory, fixture.host, fixture.requirements,
                                    fixture.selection)
                    .diagnostic.reason == NetworkTargetFailureReason::PackageMismatch);
    }
}  // namespace
