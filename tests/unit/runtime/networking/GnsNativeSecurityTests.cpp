#include "GnsTransportFactory.h"
#include "Horo/Network/NetworkErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <functional>

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

    GnsNativeSecurityOptions Options(const std::shared_ptr<RejectingIssuer> &issuer) {
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
    const auto mismatch = composition.transport->Initialize({5, 64, 1200});
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
