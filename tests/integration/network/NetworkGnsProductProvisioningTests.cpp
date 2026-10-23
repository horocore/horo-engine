#include "GnsTransportFactory.h"
#include "NetworkAdmissionProductTestSupport.h"

namespace Horo::Network::AdmissionTests {
    namespace {
        /** @brief Startup-only authority denial; no peer is authenticated by this fixture. */
        class ProductRejectingIssuer final : public IGnsCertificateIssuer {
        public:
            Result<Security::SecureBytes> Issue(std::span<const std::byte> request,
                                                std::chrono::steady_clock::time_point deadline) override {
                ++calls;
                validRequest = !request.empty() && request.size() <= 16384 && deadline > std::chrono::steady_clock::now();
                return Result<Security::SecureBytes>::Failure(
                    MakeError(NetworkErrors::AuthenticationRejected, "product provisioning denied"));
            }

            unsigned calls{};
            bool validRequest{};
        };

        /** @brief Explicit startup-denial selection, separate from any successful authentication fixture. */
        GnsNativeSecurityOptions DeniedOptions(const NetworkTrustPolicySnapshot &trust,
                                               const std::shared_ptr<ProductRejectingIssuer> &issuer) {
            GnsNativeSecurityOptions options;
            options.trustedRoot.fill(std::byte{1});  // Explicit public test root; no successful certificate or peer claim.
            options.appId = 480;
            options.trust = trust;
            options.peerTrust = NetworkTrustLevel::Paired;
            options.transport = {4, 64, 1200};
            options.provisioningBudget = std::chrono::seconds(5);
            options.issuer = issuer;
            return options;
        }
    }  // namespace

    TEST_CASE("Actual product GNS Configure precedes provisioning and preserves startup denial", "[integration][network][gns][security]") {
        ProductPair fixture;
        auto issuer = std::make_shared<ProductRejectingIssuer>();
        auto policy = fixture.Policy(0);
        policy.trust.exposure = NetworkExposure::LocalNetwork;
        policy.trust.allowUnprotectedInMemoryLoopback = false;
        auto selected = CreateSecureGnsTransport(DeniedOptions(policy.trust, issuer));
        REQUIRE(selected.HasValue());
        auto composition = std::move(selected).Value();
        policy.local.transport = composition.transport->Capabilities();
        auto providers = fixture.authorities[0]->Providers(fixture.authorities[0]);
        providers.certificates = composition.native.certificates;
        providers.peers = composition.native.peers;
        providers.privateKeys = composition.native.privateKeys;
        providers.channel = composition.native.channel;
        auto capture = Application::Internal::NetworkProductAdmission::Capture(policy, std::move(providers), fixture.codecs);
        REQUIRE(capture.HasValue());
        auto debugger = std::make_shared<Application::NetworkDebuggerService>();
        REQUIRE(debugger->Begin(1901, 1, true, NetworkDiagnosticProvider::Native));
        auto service =
            Application::Internal::CreateNetworkProductTransport(std::move(composition.transport), NetworkProjectRole::DedicatedServer, {},
                                                                 {}, debugger, fixture.owners[0]->sessions, std::move(capture).Value());
        REQUIRE(service.HasValue());  // The actual dispatcher/controller configured against the inert declaration.
        REQUIRE(issuer->calls == 0);
        const auto prepared = service.Value()->Prepare();
        REQUIRE(prepared.HasError());
        REQUIRE(prepared.ErrorValue().domain.Value() == NetworkErrors::AuthenticationRejected.domain.Value());
        REQUIRE(prepared.ErrorValue().code.Value() == NetworkErrors::AuthenticationRejected.code.Value());
        REQUIRE(prepared.ErrorValue().message == "product provisioning denied");
        REQUIRE(issuer->calls == 1);
        REQUIRE(issuer->validRequest);
        REQUIRE_FALSE(composition.native.certificates->Available());
        service.Value()->Shutdown();
        auto replacement = CreateGnsTransport();
        REQUIRE(replacement.HasValue());
        REQUIRE(replacement.Value()->Initialize({4, 64, 1200}).HasValue());
        replacement.Value()->Shutdown();
    }
}  // namespace Horo::Network::AdmissionTests
