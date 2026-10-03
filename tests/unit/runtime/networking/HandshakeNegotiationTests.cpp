#include "Horo/Network/HandshakeNegotiation.h"
#include "Horo/Network/NetworkErrors.h"
#include "NetworkTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>

namespace Horo::Network {
    using TestSupport::Connection;
    using TestSupport::RequireError;
    using TestSupport::Session;
    using TestSupport::WireIdentity;

    namespace {
        [[nodiscard]] constexpr std::size_t Index(const DeliveryPolicy policy) noexcept {
            return static_cast<std::size_t>(policy);
        }

        [[nodiscard]] constexpr std::size_t Index(const HandshakeCompression compression) noexcept {
            return static_cast<std::size_t>(compression);
        }

        struct Fixture final {
            std::array<ProtocolFeatureId, 3> localSupported{
                WireIdentity<ProtocolFeatureId>(1),
                WireIdentity<ProtocolFeatureId>(2),
                WireIdentity<ProtocolFeatureId>(3),
            };
            std::array<ProtocolFeatureId, 1> localRequired{localSupported[0]};
            std::array<ProtocolFeatureId, 3> peerSupported{
                localSupported[2],
                localSupported[0],
                WireIdentity<ProtocolFeatureId>(4),
            };
            std::array<ProtocolFeatureId, 1> peerRequired{localSupported[2]};

            [[nodiscard]] TransportCapabilities Capabilities() const {
                TransportCapabilities capabilities;
                capabilities.revision = 11;
                capabilities.delivery.fill(TransportSupport::Unsupported);
                capabilities.delivery[Index(DeliveryPolicy::ReliableOrdered)] = TransportSupport::Available;
                capabilities.delivery[Index(DeliveryPolicy::UnreliableSequenced)] = TransportSupport::Available;
                capabilities.maximumChannels = 4;
                capabilities.maximumMessageBytes = 1400;
                capabilities.deadlines = TransportSupport::Available;
                capabilities.maximumDeadlineMilliseconds = 1000;
                return capabilities;
            }

            [[nodiscard]] TransportRequirements Requirements() const {
                TransportRequirements requirements;
                requirements.requiredDelivery[Index(DeliveryPolicy::ReliableOrdered)] = true;
                requirements.requiredChannels = 2;
                requirements.requiredMaximumMessageBytes = 1200;
                requirements.deadline = DeadlineRequirement::Required;
                requirements.requiredMaximumDeadlineMilliseconds = 500;
                return requirements;
            }

            [[nodiscard]] HandshakeLocalDescriptor Local() const {
                HandshakeLocalDescriptor local;
                auto &compatibility = local.compatibility;
                compatibility.protocol = WireIdentity<ProtocolId>(10);
                compatibility.versions = {{2, 1}, {2, 6}};
                compatibility.schemaFingerprint = 0x1234;
                compatibility.features = {localSupported, localRequired};
                compatibility.compression[Index(HandshakeCompression::None)] = true;
                compatibility.compression[Index(HandshakeCompression::Lz4)] = true;
                local.transport = Capabilities();
                return local;
            }

            [[nodiscard]] HandshakeOffer Offer() const {
                HandshakeOffer offer;
                auto &compatibility = offer.compatibility;
                compatibility.protocol = WireIdentity<ProtocolId>(10);
                compatibility.versions = {{2, 3}, {2, 8}};
                compatibility.schemaFingerprint = 0x1234;
                compatibility.features = {peerSupported, peerRequired};
                compatibility.compression[Index(HandshakeCompression::None)] = true;
                compatibility.compression[Index(HandshakeCompression::Lz4)] = true;
                offer.transport = Requirements();
                return offer;
            }

            [[nodiscard]] HandshakeNegotiator Negotiator() const {
                return HandshakeNegotiator::Create(Connection(), Session(), 100, Local()).Value();
            }
        };
    }  // namespace

    TEST_CASE("Handshake selects one immutable generation-bound session snapshot", "[unit][network][handshake]") {
        const Fixture fixture;
        auto negotiator = fixture.Negotiator();
        const auto result = negotiator.Accept(Connection(), Session(), fixture.Offer(), 99);

        REQUIRE(result.HasValue());
        REQUIRE(result.Value().connection == Connection());
        REQUIRE(result.Value().sessionGeneration == Session());
        REQUIRE(result.Value().version == ProtocolVersion{2, 6});
        const std::array expectedFeatures{WireIdentity<ProtocolFeatureId>(1), WireIdentity<ProtocolFeatureId>(3)};
        REQUIRE(std::ranges::equal(result.Value().features.Values(), expectedFeatures));
        REQUIRE(result.Value().compression == HandshakeCompression::Lz4);
        REQUIRE(result.Value().transport.capabilityRevision == 11);
        REQUIRE(negotiator.State() == HandshakeState::Accepted);
        REQUIRE(negotiator.Selection() != nullptr);
        REQUIRE(*negotiator.Selection() == result.Value());

        RequireError(negotiator.Accept(Connection(), Session(), fixture.Offer(), 99), NetworkErrors::HandshakeStateInvalid);
        REQUIRE(*negotiator.Selection() == result.Value());
    }

    TEST_CASE("Handshake never downgrades required features compression or transport semantics", "[unit][network][handshake]") {
        Fixture fixture;
        fixture.peerSupported[1] = fixture.peerSupported[2];
        RequireError(fixture.Negotiator().Accept(Connection(), Session(), fixture.Offer(), 1), NetworkErrors::HandshakeInvalid);

        fixture = Fixture{};
        fixture.peerSupported[1] = WireIdentity<ProtocolFeatureId>(5);
        RequireError(fixture.Negotiator().Accept(Connection(), Session(), fixture.Offer(), 1), NetworkErrors::HandshakeIncompatible);

        fixture = Fixture{};
        auto local = fixture.Local();
        local.compatibility.requiredCompression = HandshakeCompression::Lz4;
        auto peer = fixture.Offer();
        peer.compatibility.compression[Index(HandshakeCompression::Lz4)] = false;
        auto negotiator = HandshakeNegotiator::Create(Connection(), Session(), 100, local).Value();
        RequireError(negotiator.Accept(Connection(), Session(), peer, 1), NetworkErrors::HandshakeIncompatible);

        fixture = Fixture{};
        peer = fixture.Offer();
        peer.transport.requiredMaximumMessageBytes = fixture.Capabilities().maximumMessageBytes + 1;
        RequireError(fixture.Negotiator().Accept(Connection(), Session(), peer, 1), NetworkErrors::TransportLimitExceeded);
    }

    TEST_CASE("Handshake rejects malformed and hostile bounded declarations before acceptance", "[unit][network][handshake]") {
        Fixture fixture;
        auto local = fixture.Local();
        local.compatibility.contractVersion = 0;
        RequireError(HandshakeNegotiator::Create(Connection(), Session(), 100, local), NetworkErrors::HandshakeInvalid);

        local = fixture.Local();
        local.compatibility.features.required = {fixture.localSupported.data(), 2};
        fixture.localSupported[1] = fixture.localSupported[0];
        RequireError(HandshakeNegotiator::Create(Connection(), Session(), 100, local), NetworkErrors::HandshakeInvalid);

        fixture = Fixture{};
        std::array<ProtocolFeatureId, MaximumHandshakeFeatures + 1> oversized{};
        oversized.fill(WireIdentity<ProtocolFeatureId>(1));
        auto offer = fixture.Offer();
        offer.compatibility.features.supported = oversized;
        RequireError(fixture.Negotiator().Accept(Connection(), Session(), offer, 1), NetworkErrors::HandshakeInvalid);

        offer = fixture.Offer();
        offer.compatibility.protocol = WireIdentity<ProtocolId>(11);
        RequireError(fixture.Negotiator().Accept(Connection(), Session(), offer, 1), NetworkErrors::HandshakeIncompatible);

        offer = fixture.Offer();
        offer.compatibility.versions = {{3, 0}, {3, 2}};
        RequireError(fixture.Negotiator().Accept(Connection(), Session(), offer, 1), NetworkErrors::HandshakeIncompatible);

        offer = fixture.Offer();
        offer.compatibility.schemaFingerprint = 0x9999;
        RequireError(fixture.Negotiator().Accept(Connection(), Session(), offer, 1), NetworkErrors::HandshakeIncompatible);
    }

    TEST_CASE("Handshake timeout cancellation shutdown and replacement generations are terminal", "[unit][network][handshake]") {
        const Fixture fixture;
        auto stale = fixture.Negotiator();
        RequireError(stale.Accept(Connection(4), Session(), fixture.Offer(), 1), NetworkErrors::NetworkLifecycleOperationStale);
        REQUIRE(stale.State() == HandshakeState::AwaitingOffer);
        RequireError(stale.Accept(Connection(), Session(8), fixture.Offer(), 1), NetworkErrors::NetworkLifecycleOperationStale);

        auto cancelled = fixture.Negotiator();
        RequireError(cancelled.Accept(Connection(), Session(), fixture.Offer(), 1, TransportAdmissionState::Cancelled),
                     NetworkErrors::SessionCancelled);
        REQUIRE(cancelled.State() == HandshakeState::Rejected);

        auto timedOut = fixture.Negotiator();
        RequireError(timedOut.Accept(Connection(), Session(), fixture.Offer(), 100), NetworkErrors::SessionTimedOut);
        REQUIRE(timedOut.State() == HandshakeState::TimedOut);
        REQUIRE_FALSE(timedOut.Expire(101));

        auto expired = fixture.Negotiator();
        REQUIRE_FALSE(expired.Expire(99));
        REQUIRE(expired.Expire(100));
        REQUIRE_FALSE(expired.Expire(100));
        REQUIRE(expired.Selection() == nullptr);

        auto shutdown = fixture.Negotiator();
        REQUIRE(shutdown.Shutdown());
        REQUIRE_FALSE(shutdown.Shutdown());
        RequireError(shutdown.Accept(Connection(), Session(), fixture.Offer(), 1), NetworkErrors::SessionShuttingDown);

        auto rejected = fixture.Negotiator();
        REQUIRE(rejected.Reject(Connection(), Session()).HasValue());
        RequireError(rejected.Reject(Connection(), Session()), NetworkErrors::HandshakeStateInvalid);
        REQUIRE_FALSE(rejected.Shutdown());
    }

    TEST_CASE("Handshake qualifies the complete supported minor-version window without downgrade",
              "[unit][network][handshake][qualification]") {
        const Fixture fixture;
        for (std::uint16_t minimum = 0; minimum <= 8; ++minimum) {
            for (std::uint16_t maximum = minimum; maximum <= 8; ++maximum) {
                auto offer = fixture.Offer();
                offer.compatibility.versions = {{2, minimum}, {2, maximum}};
                auto negotiator = fixture.Negotiator();
                const auto result = negotiator.Accept(Connection(), Session(), offer, 1);
                const bool compatible = maximum >= 1 && minimum <= 6;
                if (compatible) {
                    REQUIRE(result.HasValue());
                    REQUIRE(result.Value().version == ProtocolVersion{2, std::min<std::uint16_t>(maximum, 6)});
                    REQUIRE(negotiator.State() == HandshakeState::Accepted);
                } else {
                    RequireError(result, NetworkErrors::HandshakeIncompatible);
                    REQUIRE(negotiator.State() == HandshakeState::Rejected);
                    REQUIRE(negotiator.Selection() == nullptr);
                    RequireError(negotiator.Accept(Connection(), Session(), fixture.Offer(), 2), NetworkErrors::HandshakeStateInvalid);
                }
            }
        }
    }
}  // namespace Horo::Network
