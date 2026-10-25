#include "GnsNativeCall.h"
#include "GnsNativeHost.h"
#include "GnsSecurityInternal.h"
#include "GnsTransportFactory.h"
#include "GnsTransportInternal.h"
#include "Horo/Network/NetworkErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <functional>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Network;

    class RejectingIssuer final : public IGnsCertificateIssuer {
    public:
        Result<Security::SecureBytes> Issue(std::span<const std::byte> request, std::chrono::steady_clock::time_point deadline) override {
            REQUIRE(!request.empty());
            REQUIRE(request.size() <= 16384);
            REQUIRE(deadline > std::chrono::steady_clock::now());
            ++calls;
            if (action)
                action();
            return Result<Security::SecureBytes>::Failure(MakeError(NetworkErrors::AuthenticationRejected, "qualification issuer denial"));
        }

        std::function<void()> action;
        unsigned calls{};
    };

    GnsNativeSecurityOptions Options(const std::shared_ptr<IGnsCertificateIssuer> &issuer) {
        GnsNativeSecurityOptions options;
        // Explicit public qualification root; these cases never authenticate a peer.
        options.trustedRoot.fill(std::byte{1});
        options.appId = 480;
        options.trust = {AuthenticationContractVersion,
                         NetworkTrustPolicyId::Create(1).Value(),
                         1,
                         NetworkExposure::LocalNetwork,
                         CredentialBindingId::Create(2).Value(),
                         CertificateBindingId::Create(3).Value(),
                         PrivateKeyBindingId::Create(4).Value(),
                         64,
                         false};
        options.peerTrust = NetworkTrustLevel::Paired;
        options.transport = {4, 64, 1200};
        options.provisioningBudget = std::chrono::seconds(5);
        options.issuer = issuer;
        return options;
    }

    void RequireRawOwnerReusable() {
        auto raw = CreateGnsTransport();
        REQUIRE(raw.HasValue());
        const auto initialized = raw.Value()->Initialize({4, 64, 1200});
        if (initialized.HasError())
            INFO(initialized.ErrorValue().domain.Value() << ':' << initialized.ErrorValue().code.Value());
        REQUIRE(initialized.HasValue());
        raw.Value()->Shutdown();
    }
}  // namespace

TEST_CASE("Secure GNS captures inert exact capabilities without provisioning", "[unit][network][gns][security]") {
    auto issuer = std::make_shared<RejectingIssuer>();
    auto selected = CreateSecureGnsTransport(Options(issuer));
    REQUIRE(selected.HasValue());
    auto &composition = selected.Value();
    const auto declaration = composition.transport->Capabilities();
    REQUIRE(declaration.revision == 1);
    REQUIRE(declaration.maximumChannels == 1);
    REQUIRE(declaration.maximumMessageBytes == 1200);
    REQUIRE(declaration.delivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] == TransportSupport::Available);
    REQUIRE_FALSE(composition.native.certificates->Available());
    REQUIRE_FALSE(composition.native.peers->Available());
    REQUIRE_FALSE(composition.native.privateKeys->Available());
    REQUIRE(issuer->calls == 0);
    auto incompatible = Options(issuer).transport;
    const auto field = GENERATE(0, 1, 2);
    CAPTURE(field);
    if (field == 0)
        ++incompatible.maximumConnections;
    else if (field == 1)
        ++incompatible.maximumEventsPerPoll;
    else
        --incompatible.maximumMessageBytes;  // Valid native ceiling, distinct from the captured declaration.
    REQUIRE(ValidGnsTransportConfig(incompatible));
    const auto mismatch = composition.transport->Initialize(incompatible);
    REQUIRE(mismatch.HasError());
    REQUIRE(mismatch.ErrorValue().domain.Value() == NetworkErrors::TransportBackendConflict.domain.Value());
    REQUIRE(mismatch.ErrorValue().code.Value() == NetworkErrors::TransportBackendConflict.code.Value());
    REQUIRE(composition.transport->Capabilities() == declaration);
    REQUIRE(issuer->calls == 0);
}

TEST_CASE("Secure GNS issuer runs outside both locks and preserves typed failure", "[unit][network][gns][security]") {
    auto issuer = std::make_shared<RejectingIssuer>();
    auto selected = CreateSecureGnsTransport(Options(issuer));
    REQUIRE(selected.HasValue());
    auto &composition = selected.Value();
    issuer->action = [&] {
        REQUIRE(composition.transport->Stats().activeConnections == 0);
        auto competing = CreateGnsTransport();
        REQUIRE(competing.HasValue());
        const auto unavailable = competing.Value()->Initialize({4, 64, 1200});
        REQUIRE(unavailable.HasError());
        REQUIRE(unavailable.ErrorValue().domain.Value() == NetworkErrors::TransportCapabilityUnavailable.domain.Value());
        REQUIRE(unavailable.ErrorValue().code.Value() == NetworkErrors::TransportCapabilityUnavailable.code.Value());
    };
    const auto rejected = composition.transport->Initialize({4, 64, 1200});
    REQUIRE(rejected.HasError());
    REQUIRE(rejected.ErrorValue().domain.Value() == NetworkErrors::AuthenticationRejected.domain.Value());
    REQUIRE(rejected.ErrorValue().code.Value() == NetworkErrors::AuthenticationRejected.code.Value());
    REQUIRE(rejected.ErrorValue().message == "qualification issuer denial");
    REQUIRE(issuer->calls == 1);
    REQUIRE_FALSE(composition.native.certificates->Available());
    REQUIRE_FALSE(composition.native.privateKeys->Available());
    composition.transport->Shutdown();
    RequireRawOwnerReusable();
}

TEST_CASE("Secure GNS reentrant startup shutdown cannot publish native authority", "[unit][network][gns][security]") {
    auto issuer = std::make_shared<RejectingIssuer>();
    auto selected = CreateSecureGnsTransport(Options(issuer));
    REQUIRE(selected.HasValue());
    auto &composition = selected.Value();
    issuer->action = [&] {
        composition.transport->Shutdown();
    };
    const auto stopped = composition.transport->Initialize({4, 64, 1200});
    REQUIRE(stopped.HasError());
    REQUIRE(stopped.ErrorValue().domain.Value() == NetworkErrors::TransportShuttingDown.domain.Value());
    REQUIRE(stopped.ErrorValue().code.Value() == NetworkErrors::TransportShuttingDown.code.Value());
    REQUIRE(issuer->calls == 1);
    REQUIRE_FALSE(composition.native.peers->Available());
    composition.transport->Shutdown();
    RequireRawOwnerReusable();
}

namespace {
    enum class IssuerOutcome {
        ThrowStandard,
        ThrowForeign,
        Empty,
        Oversized,
        Expired,
        InvalidCertificate
    };

    class BoundaryIssuer final : public IGnsCertificateIssuer {
    public:
        explicit BoundaryIssuer(IssuerOutcome outcome) : outcome_(outcome) {}

        Result<Security::SecureBytes> Issue(std::span<const std::byte>, std::chrono::steady_clock::time_point deadline) override {
            using enum IssuerOutcome;
            ++calls;
            switch (outcome_) {
                case ThrowStandard:
                    throw std::runtime_error("qualification provider failure");
                case ThrowForeign:
                    throw 7;
                case Expired:
                    std::this_thread::sleep_until(deadline);
                    break;
                case Empty:
                    break;
                case Oversized: {
                    std::vector<std::byte> bytes(16385, std::byte{1});
                    return Result<Security::SecureBytes>::Success(Security::SecureBytes(bytes));
                }
                case InvalidCertificate: {
                    const std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}};
                    return Result<Security::SecureBytes>::Success(Security::SecureBytes(bytes));
                }
            }
            return Result<Security::SecureBytes>::Success(Security::SecureBytes{});
        }

        unsigned calls{};

    private:
        IssuerOutcome outcome_;
    };

    void RequireCode(const Error &error, const ErrorCodeDescriptor &expected) {
        REQUIRE(error.domain.Value() == expected.domain.Value());
        REQUIRE(error.code.Value() == expected.code.Value());
    }
}  // namespace

TEST_CASE("Native foreign-call barriers contain standard and nonstandard exceptions without allocating an error",
          "[unit][network][gns][security]") {
    REQUIRE(GnsDetail::NativeCall([] {
        return 17;
    }).value() == 17);
    REQUIRE_FALSE(GnsDetail::NativeCall([]() -> int {
        throw std::runtime_error("native failure");
    }).has_value());
    REQUIRE_FALSE(GnsDetail::NativeCall([]() -> int {
        throw 19;
    }).has_value());
    unsigned calls{};
    REQUIRE(GnsDetail::NativeVoid([&calls] {
        ++calls;
    }));
    REQUIRE(calls == 1);
    REQUIRE_FALSE(GnsDetail::NativeVoid([] {
        throw std::runtime_error("native cleanup failure");
    }));
    REQUIRE_FALSE(GnsDetail::NativeVoid([] {
        throw 23;
    }));
}

TEST_CASE("Native bridge terminal classifications retain their exact canonical network descriptor", "[unit][network][gns][security]") {
    using enum SteamNetworkingSocketsLib::HoroBridgeStatus;
    const std::array cases{std::pair{Stale, &NetworkErrors::TerminalGenerationStale},
                           std::pair{Untrusted, &NetworkErrors::AuthenticationRejected},
                           std::pair{Expired, &NetworkErrors::SessionTimedOut},
                           std::pair{Invalid, &NetworkErrors::AuthenticationInvalid},
                           std::pair{Unavailable, &NetworkErrors::AuthenticationTrustUnavailable},
                           std::pair{Ok, &NetworkErrors::FatalFailure}};
    for (const auto &[status, expected] : cases) {
        CAPTURE(static_cast<unsigned>(status));
        RequireCode(GnsSecurityFailure(status), *expected);
    }
}

TEST_CASE("Detached native authority views deny every operation without inferring a live peer", "[unit][network][gns][security]") {
    auto issuer = std::make_shared<RejectingIssuer>();
    auto selected = CreateSecureGnsTransport(Options(issuer));
    REQUIRE(selected.HasValue());
    const auto &providers = selected.Value().native;
    const auto connection = ConnectionHandle::Create(0, 1).Value();
    const auto generation = NetworkOperationGeneration::Create(1).Value();
    auto zeroClock = providers.channel->Inspect(connection, generation, 0);
    REQUIRE(zeroClock.HasError());
    RequireCode(zeroClock.ErrorValue(), NetworkErrors::AuthenticationInvalid);
    auto inspect = providers.channel->Inspect(connection, generation, 1);
    REQUIRE(inspect.HasError());
    RequireCode(inspect.ErrorValue(), NetworkErrors::AuthenticationTrustUnavailable);
    auto certificate = providers.certificates->Verify(CertificateVerificationRequest{});
    auto peer = providers.peers->Verify(PeerVerificationRequest{});
    auto binding = providers.privateKeys->Bind(SecureChannelBindingRequest{});
    REQUIRE(certificate.HasError());
    REQUIRE(peer.HasError());
    REQUIRE(binding.HasError());
    RequireCode(certificate.ErrorValue(), NetworkErrors::AuthenticationTrustUnavailable);
    RequireCode(peer.ErrorValue(), NetworkErrors::AuthenticationTrustUnavailable);
    RequireCode(binding.ErrorValue(), NetworkErrors::AuthenticationTrustUnavailable);
    selected.Value().transport->Shutdown();
    const auto restarted = selected.Value().transport->Initialize({4, 64, 1200});
    REQUIRE(restarted.HasError());
    RequireCode(restarted.ErrorValue(), NetworkErrors::TransportShuttingDown);
    REQUIRE_FALSE(providers.certificates->Available());
    REQUIRE(issuer->calls == 0);
}

TEST_CASE("Native startup issuer boundaries reject exceptions expired and malformed owned results", "[unit][network][gns][security]") {
    const auto outcome = GENERATE(IssuerOutcome::ThrowStandard, IssuerOutcome::ThrowForeign, IssuerOutcome::Empty, IssuerOutcome::Oversized,
                                  IssuerOutcome::Expired, IssuerOutcome::InvalidCertificate);
    CAPTURE(static_cast<unsigned>(outcome));
    auto issuer = std::make_shared<BoundaryIssuer>(outcome);
    auto options = Options(issuer);
    options.provisioningBudget = std::chrono::milliseconds(10);
    auto selected = CreateSecureGnsTransport(options);
    REQUIRE(selected.HasValue());
    const auto initialized = selected.Value().transport->Initialize(options.transport);
    REQUIRE(initialized.HasError());
    const auto *expected = &NetworkErrors::AuthenticationInvalid;
    if (outcome == IssuerOutcome::ThrowStandard || outcome == IssuerOutcome::ThrowForeign)
        expected = &NetworkErrors::AuthenticationTrustUnavailable;
    else if (outcome == IssuerOutcome::Expired)
        expected = &NetworkErrors::SessionTimedOut;
    else if (outcome == IssuerOutcome::InvalidCertificate)
        expected = &NetworkErrors::AuthenticationRejected;
    RequireCode(initialized.ErrorValue(), *expected);
    REQUIRE(issuer->calls == 1);
    REQUIRE_FALSE(selected.Value().native.certificates->Available());
    selected.Value().transport->Shutdown();
    RequireRawOwnerReusable();
}

TEST_CASE("Secure native selection rejects absent roots invalid policy and incompatible trust before provisioning",
          "[unit][network][gns][security]") {
    auto issuer = std::make_shared<RejectingIssuer>();
    auto options = Options(issuer);
    SECTION("absent root") {
        options.trustedRoot.fill(std::byte{});
    }
    SECTION("absent issuer") {
        options.issuer.reset();
    }
    SECTION("invalid application") {
        options.appId = 0;
    }
    SECTION("invalid policy") {
        options.trust.revision = 0;
    }
    SECTION("remote exposure requires selected product anchor") {
        options.trust.exposure = NetworkExposure::Remote;
    }
    SECTION("local exposure rejects development authority") {
        options.peerTrust = NetworkTrustLevel::LocalDevelopment;
    }
    SECTION("unknown authority") {
        options.peerTrust = NetworkTrustLevel::Count;
    }
    SECTION("invalid declared bounds") {
        options.transport.maximumEventsPerPoll = 0;
    }
    SECTION("zero provisioning deadline") {
        options.provisioningBudget = std::chrono::milliseconds(0);
    }
    SECTION("unbounded provisioning deadline") {
        options.provisioningBudget = std::chrono::seconds(31);
    }
    const auto selected = CreateSecureGnsTransport(options);
    REQUIRE(selected.HasError());
    RequireCode(selected.ErrorValue(), NetworkErrors::AuthenticationInvalid);
    REQUIRE(issuer->calls == 0);
}

namespace {
    unsigned coldKillCalls{};

    void ThrowBeforeNativeKill() {
        ++coldKillCalls;
        throw std::runtime_error("qualification cold native retirement failure");
    }
}  // namespace

TEST_CASE("Failed cold native Kill transfers retry ownership once and an old adapter cannot kill its replacement",
          "[unit][network][gns][security]") {
    coldKillCalls = 0;
    auto old = std::make_unique<GnsTransport>(ThrowBeforeNativeKill);
    REQUIRE(old->Initialize({4, 64, 1200}).HasValue());
    old->Shutdown();
    REQUIRE(coldKillCalls == 1);
    {
        auto &owner = NativeHost();
        std::lock_guard lock(owner.mutex);
        REQUIRE(owner.nativeCleanupPending);
        REQUIRE(owner.shuttingDown);
        REQUIRE_FALSE(owner.cleanupRunning);
        REQUIRE(owner.active == nullptr);
    }
    auto replacement = CreateGnsTransport();
    REQUIRE(replacement.HasValue());
    REQUIRE(replacement.Value()->Initialize({4, 64, 1200}).HasValue());
    {
        auto &owner = NativeHost();
        std::lock_guard lock(owner.mutex);
        REQUIRE_FALSE(owner.nativeCleanupPending);
        REQUIRE_FALSE(owner.shuttingDown);
        REQUIRE(owner.active != nullptr);
    }
    old->Shutdown();
    old.reset();
    REQUIRE(coldKillCalls == 1);
    const auto endpoint = NetworkAddress::Parse("127.0.0.1:48539");
    REQUIRE(endpoint.HasValue());
    auto listener = replacement.Value()->Listen({endpoint.Value(), 1});
    REQUIRE(listener.HasValue());
    REQUIRE(replacement.Value()->CloseListener(listener.Value()).HasValue());
    replacement.Value()->Shutdown();
}

TEST_CASE("Native preparation preserves an actual bridge policy rejection without invoking the issuer", "[unit][network][gns][security]") {
    auto raw = CreateGnsTransport();
    REQUIRE(raw.HasValue());
    REQUIRE(raw.Value()->Initialize({4, 64, 1200}).HasValue());
    auto issuer = std::make_shared<RejectingIssuer>();
    auto invalid = Options(issuer);
    invalid.appId = 0;
    // Lower-boundary rejection qualification only: the public factory rejects this
    // input earlier. No peer, activation, trusted root or successful product is fabricated.
    GnsSecurity security(std::move(invalid));
    std::uint64_t generation{};
    auto *native = SteamNetworkingSockets();
    REQUIRE(native != nullptr);
    const auto request = security.Prepare(*native, generation);
    REQUIRE(request.HasError());
    RequireCode(request.ErrorValue(), NetworkErrors::AuthenticationInvalid);
    REQUIRE(generation == 0);
    REQUIRE(issuer->calls == 0);
    raw.Value()->Shutdown();
    RequireRawOwnerReusable();
}

namespace {
    unsigned cleanupProbeCalls{};
    bool cleanupProbeDenied{};

    /** @brief Probe actual process cleanup fencing, then delegate to the real native Kill exactly once. */
    void ProbeBeforeNativeKill() {
        ++cleanupProbeCalls;
        auto competing = CreateGnsTransport();
        if (competing.HasValue()) {
            const auto result = competing.Value()->Initialize({4, 64, 1200});
            cleanupProbeDenied = result.HasError() &&
                                 result.ErrorValue().domain.Value() == NetworkErrors::AuthenticationTrustUnavailable.domain.Value() &&
                                 result.ErrorValue().code.Value() == NetworkErrors::AuthenticationTrustUnavailable.code.Value();
        }
        GameNetworkingSockets_Kill();
    }
}  // namespace

TEST_CASE("Native active and retiring owners deny competing admission without releasing the real owner", "[unit][network][gns][security]") {
    cleanupProbeCalls = 0;
    cleanupProbeDenied = false;
    auto owner = std::make_unique<GnsTransport>(ProbeBeforeNativeKill);
    REQUIRE(owner->Initialize({4, 64, 1200}).HasValue());
    auto competing = CreateGnsTransport();
    REQUIRE(competing.HasValue());
    const auto occupied = competing.Value()->Initialize({4, 64, 1200});
    REQUIRE(occupied.HasError());
    RequireCode(occupied.ErrorValue(), NetworkErrors::TransportCapabilityUnavailable);
    competing.Value()->Shutdown();
    const auto endpoint = NetworkAddress::Parse("127.0.0.1:48540");
    REQUIRE(endpoint.HasValue());
    const auto listener = owner->Listen({endpoint.Value(), 1});
    REQUIRE(listener.HasValue());
    REQUIRE(owner->CloseListener(listener.Value()).HasValue());
    owner->Shutdown();
    REQUIRE(cleanupProbeCalls == 1);
    REQUIRE(cleanupProbeDenied);
    auto replacement = CreateGnsTransport();
    REQUIRE(replacement.HasValue());
    REQUIRE(replacement.Value()->Initialize({4, 64, 1200}).HasValue());
    owner.reset();
    REQUIRE(cleanupProbeCalls == 1);
    const auto replacementListener = replacement.Value()->Listen({endpoint.Value(), 1});
    REQUIRE(replacementListener.HasValue());
    REQUIRE(replacement.Value()->CloseListener(replacementListener.Value()).HasValue());
    replacement.Value()->Shutdown();
}

TEST_CASE("Native initialization rejects hostile resource ceilings before acquiring a process owner", "[unit][network][gns][security]") {
    const auto field = GENERATE(0, 1, 2);
    CAPTURE(field);
    NetworkTransportConfig oversized{4, 64, 1200};
    if (field == 0)
        oversized.maximumConnections = std::numeric_limits<std::uint32_t>::max();
    else if (field == 1)
        oversized.maximumEventsPerPoll = std::numeric_limits<std::uint32_t>::max();
    else
        oversized.maximumMessageBytes = std::numeric_limits<std::uint32_t>::max();
    auto candidate = CreateGnsTransport();
    REQUIRE(candidate.HasValue());
    const auto rejected = candidate.Value()->Initialize(oversized);
    REQUIRE(rejected.HasError());
    RequireCode(rejected.ErrorValue(), NetworkErrors::TransportLimitExceeded);
    candidate.Value()->Shutdown();
    RequireRawOwnerReusable();
}
