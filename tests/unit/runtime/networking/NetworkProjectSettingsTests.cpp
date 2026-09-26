#include "Horo/Network/NetworkProjectSettings.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace {
    Horo::Network::NetworkProjectSettingsInput ValidInput(const std::uint64_t revision = 1) {
        using namespace Horo::Network;

        NetworkProjectSettingsInput input;
        input.settings = NetworkProjectSettingsId::Create(11).Value();
        input.revision = NetworkProjectSettingsRevision::Create(revision).Value();
        input.supportedRoles = NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::Client | NetworkProjectRoleSet::ListenServer |
                               NetworkProjectRoleSet::DedicatedServer;
        input.defaultRole = NetworkProjectRole::Standalone;

        input.profile.id = NetworkProjectProfileId::Create(21).Value();
        input.profile.revision = NetworkProjectProfileRevision::Create(revision).Value();
        input.profile.networkTickRate = {.value = 60};
        input.profile.maxActiveConnections = {.value = 128};
        input.profile.maxNetworkObjects = {.value = 10'000};
        input.profile.maxCandidatesPerConnectionPerTick = {.value = 2'000};
        input.profile.maxRelevantObjectsPerConnection = {.value = 1'000};
        input.profile.maxInterestTransitionsPerConnectionPerTick = {.value = 256};
        input.profile.maxCapturedObjectsPerTick = {.value = 2'000};
        input.profile.maxSerializedFieldsPerConnectionPerTick = {.value = 8'000};
        input.profile.maxGlobalInterestWorkPerTick = {.value = 2'000};
        input.profile.maxGlobalCaptureWorkPerTick = {.value = 2'000};
        input.profile.maxGlobalSchedulingWorkPerTick = {.value = 2'000};
        input.profile.targetBytesPerConnection = {.value = 64'000};
        input.profile.burstBytesPerConnection = {.value = 128'000};
        input.profile.maxMessagesPerConnectionPerTick = {.value = 256};
        input.profile.maxReliableQueuedBytesPerConnection = {.value = 256'000};
        input.profile.maxUnreliableQueuedBytesPerConnection = {.value = 256'000};
        input.profile.enterDwellTicks = {.value = 2};
        input.profile.exitDwellTicks = {.value = 2};
        input.profile.maxEligibleStarvationTicks = {.value = 120};
        input.profile.saturationGraceTicks = {.value = 30};

        input.protocol.protocol = ProtocolId::Create(1).Value();
        input.protocol.supportedVersions = {.minimum = {.major = 1, .minor = 0}, .maximum = {.major = 1, .minor = 3}};
        input.protocol.schemaFingerprint = 0x1122'3344'5566'7788ULL;

        input.transport.requirement = NetworkProjectTransportRequirement::Required;
        input.transport.capabilities.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
        input.transport.capabilities.requiredChannels = 2;
        input.transport.capabilities.requiredMaximumMessageBytes = 64 * 1024;
        return input;
    }

    TEST_CASE("Network project settings are an immutable typed authority", "[unit][network][settings]") {
        using namespace Horo::Network;

        const auto input = ValidInput();
        const auto result = NetworkProjectSettings::Create(input);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().Settings().Value() == 11);
        REQUIRE(result.Value().Revision().Value() == 1);
        REQUIRE(result.Value().DefaultRole() == NetworkProjectRole::Standalone);
        REQUIRE(result.Value().Profile().networkTickRate.value == 60);
        REQUIRE(result.Value().Protocol().schemaFingerprint == input.protocol.schemaFingerprint);
        REQUIRE(result.Value().Transport().requirement == NetworkProjectTransportRequirement::Required);
        REQUIRE(result.Value().Fingerprint().IsValid());

        NetworkPreviewPreferences preview;
        REQUIRE(preview.IsValid());
        preview.maxPreviewClients = 16;
        preview.simulatedLatencyMilliseconds = 500;
        REQUIRE(preview.IsValid());
        preview.maxPreviewClients = 17;
        REQUIRE_FALSE(preview.IsValid());
    }

    TEST_CASE("Network project settings reject invalid roles, protocol and capacity", "[unit][network][settings]") {
        using namespace Horo::Network;

        auto input = ValidInput();
        input.supportedRoles = NetworkProjectRoleSet::None;
        auto result = NetworkProjectSettings::Create(input);
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == "network.project_settings.invalid");

        input = ValidInput();
        input.profile.maxRelevantObjectsPerConnection.value = 2'001;
        result = NetworkProjectSettings::Create(input);
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == "network.project_settings.capacity_exceeded");

        input = ValidInput();
        input.protocol.protocol = {};
        result = NetworkProjectSettings::Create(input);
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == "network.project_settings.invalid");
    }

    TEST_CASE("Network project settings authority preserves snapshots across stale and shutdown commands", "[unit][network][settings]") {
        using namespace Horo::Network;

        auto authorityResult = NetworkProjectSettingsAuthority::Create(ValidInput());
        REQUIRE(authorityResult.HasValue());
        auto authority = std::move(authorityResult).Value();
        const auto original = authority.Snapshot();

        auto successor = ValidInput(2);
        successor.profile.revision = NetworkProjectProfileRevision::Create(2).Value();
        auto applied = authority.Apply({.expectedRevision = original.settings.Revision(), .candidate = successor});
        REQUIRE(applied.HasValue());
        REQUIRE(applied.Value().settings.Revision().Value() == 2);

        auto stale = ValidInput(3);
        stale.profile.revision = NetworkProjectProfileRevision::Create(3).Value();
        const auto staleResult = authority.Apply({.expectedRevision = original.settings.Revision(), .candidate = stale});
        REQUIRE(staleResult.HasError());
        REQUIRE(staleResult.ErrorValue().code.Value() == "network.project_settings.stale");
        REQUIRE(authority.Snapshot().settings.Revision().Value() == 2);

        authority.Shutdown();
        REQUIRE(authority.Lifecycle() == NetworkProjectSettingsLifecycle::Closed);
        const auto closedResult = authority.Apply({
            .expectedRevision = NetworkProjectSettingsRevision::Create(2).Value(),
            .candidate = stale,
        });
        REQUIRE(closedResult.HasError());
        REQUIRE(closedResult.ErrorValue().code.Value() == "network.project_settings.shutting_down");
        REQUIRE(authority.Snapshot().settings.Revision().Value() == 2);
    }

    TEST_CASE("Network project schema round-trips and migrates legacy defaults deterministically", "[unit][network][settings]") {
        using namespace Horo::Network;
        auto input = ValidInput();
        input.defaultEndpoint = NetworkAddress::Parse("example.com:31337").Value();
        input.credentialRequirementId = 42;
        const auto original = NetworkProjectSettings::Create(input);
        REQUIRE(original.HasValue());
        const auto encoded = SerializeNetworkProjectSettings(original.Value());
        const auto decoded = ParseNetworkProjectSettings(encoded);
        REQUIRE(decoded.HasValue());
        const auto roundTrip = NetworkProjectSettings::Create(decoded.Value());
        REQUIRE(roundTrip.HasValue());
        REQUIRE(SerializeNetworkProjectSettings(roundTrip.Value()) == encoded);
        REQUIRE(roundTrip.Value().Fingerprint() == original.Value().Fingerprint());
        for (const auto endpointText : {"192.0.2.1:31337", "[2001:db8::1]:31337"}) {
            auto alternate = input;
            alternate.defaultEndpoint = NetworkAddress::Parse(endpointText).Value();
            const auto candidate = NetworkProjectSettings::Create(alternate);
            REQUIRE(candidate.HasValue());
            const auto parsed = ParseNetworkProjectSettings(SerializeNetworkProjectSettings(candidate.Value()));
            REQUIRE(parsed.HasValue());
            REQUIRE(parsed.Value().defaultEndpoint == alternate.defaultEndpoint);
        }

        auto legacy = nlohmann::json::parse(encoded);
        legacy["contractVersion"] = 1;
        legacy.erase("defaultEndpoint");
        legacy.erase("credentialRequirementId");
        const auto migrated = ParseNetworkProjectSettings(legacy.dump());
        REQUIRE(migrated.HasValue());
        REQUIRE(migrated.Value().contractVersion == 2);
        REQUIRE(migrated.Value().credentialRequirementId == 0);
        REQUIRE_FALSE(migrated.Value().defaultEndpoint.IsValid());
        const auto migratedAuthority = NetworkProjectSettings::Create(migrated.Value());
        REQUIRE(migratedAuthority.HasValue());
        REQUIRE(SerializeNetworkProjectSettings(migratedAuthority.Value()) ==
                SerializeNetworkProjectSettings(NetworkProjectSettings::Create(migrated.Value()).Value()));
    }

    TEST_CASE("Invalid schema and capability preflight preserve the accepted configuration", "[unit][network][settings]") {
        using namespace Horo::Network;
        auto authorityResult = NetworkProjectSettingsAuthority::Create(ValidInput());
        REQUIRE(authorityResult.HasValue());
        auto authority = std::move(authorityResult).Value();
        const auto prior = authority.Snapshot().settings.Fingerprint();
        auto invalid = nlohmann::json::parse(SerializeNetworkProjectSettings(authority.Snapshot().settings));
        invalid["profile"]["maxRelevantObjectsPerConnection"] = 3000;
        REQUIRE(ParseNetworkProjectSettings(invalid.dump()).HasError());
        invalid = nlohmann::json::parse(SerializeNetworkProjectSettings(authority.Snapshot().settings));
        invalid["credentialReference"] = "private-store-ref";
        REQUIRE(ParseNetworkProjectSettings(invalid.dump()).HasError());
        REQUIRE(ParseNetworkProjectSettings("{\"contractVersion\":1,\"contractVersion\":1}").HasError());
        invalid = nlohmann::json::parse(SerializeNetworkProjectSettings(authority.Snapshot().settings));
        invalid["contractVersion"] = 3;
        REQUIRE(ParseNetworkProjectSettings(invalid.dump()).HasError());
        invalid["contractVersion"] = 2;
        invalid["profile"]["networkTickRate"] = -1;
        REQUIRE(ParseNetworkProjectSettings(invalid.dump()).HasError());
        REQUIRE(ParseNetworkProjectSettings(std::string(65'537, 'x')).HasError());
        REQUIRE(authority.Snapshot().settings.Fingerprint() == prior);

        auto successor = ValidInput(2);
        successor.profile.maxRelevantObjectsPerConnection.value = 3000;
        const auto rejected = authority.Apply({.expectedRevision = authority.Snapshot().settings.Revision(), .candidate = successor});
        REQUIRE(rejected.HasError());
        REQUIRE(authority.Snapshot().settings.Fingerprint() == prior);

        TransportCapabilities unavailable;
        unavailable.revision = 1;
        const auto release =
            PreflightNetworkProjectSettings(authority.Snapshot().settings, NetworkProjectRole::DedicatedServer, unavailable);
        REQUIRE(release.HasError());
        REQUIRE(PreflightNetworkProjectSettings(authority.Snapshot().settings, NetworkProjectRole::Standalone, unavailable).HasValue());
        REQUIRE(authority.Snapshot().settings.Fingerprint() == prior);
    }

    TEST_CASE("Standalone defaults and release automation enforce exact packaged roles", "[unit][network][settings]") {
        using namespace Horo::Network;
        const auto defaults = DefaultNetworkProjectSettings(NetworkProjectSettingsId::Create(99).Value());
        REQUIRE(defaults.HasValue());
        const auto accepted = NetworkProjectSettings::Create(defaults.Value());
        REQUIRE(accepted.HasValue());
        REQUIRE(accepted.Value().ContractVersion() == NetworkProjectSettingsInput::CurrentContractVersion);
        REQUIRE(ParseNetworkProjectSettings(SerializeNetworkProjectSettings(accepted.Value())).HasValue());
        REQUIRE(SerializeNetworkProjectSettings(accepted.Value()).find("credentialReference") == std::string::npos);

        TransportCapabilities unavailable;
        unavailable.revision = 1;
        REQUIRE(PreflightNetworkProjectSettings(accepted.Value(), NetworkProjectRole::Standalone, unavailable).HasValue());
        REQUIRE(PreflightNetworkProjectSettings(accepted.Value(), NetworkProjectRole::DedicatedServer, unavailable).HasError());

        auto packaged = ValidInput();
        packaged.supportedRoles = NetworkProjectRoleSet::Client;
        packaged.defaultRole = NetworkProjectRole::Client;
        const auto packageSettings = NetworkProjectSettings::Create(packaged);
        REQUIRE(packageSettings.HasValue());
        REQUIRE(PreflightNetworkProjectSettings(packageSettings.Value(), NetworkProjectRole::DedicatedServer, unavailable).HasError());
        REQUIRE(PreflightNetworkProjectSettings(packageSettings.Value(), NetworkProjectRole::Client, unavailable).HasError());
    }
}  // namespace
