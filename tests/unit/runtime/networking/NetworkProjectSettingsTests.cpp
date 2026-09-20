#include "Horo/Network/NetworkProjectSettings.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
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
}  // namespace
