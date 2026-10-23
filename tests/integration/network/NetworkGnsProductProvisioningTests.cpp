#include "GnsTransportFactory.h"
#include "NetworkAdmissionProductTestSupport.h"
#include "NetworkProductGnsSelection.h"

#include <catch2/generators/catch_generators.hpp>

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

        /** @brief Alter one independent native owner or exact trust handoff field while other inputs remain valid. */
        bool MismatchAuthority(const unsigned mismatch, NetworkAdmissionProviders &providers, const NetworkAdmissionProviders &occupied,
                               const NetworkAdmissionPolicy &policy, GnsNativeSecurityOptions &options) {
            switch (mismatch) {
                case 0:
                    providers.certificates = occupied.certificates;
                    return true;
                case 7:
                    providers.peers = occupied.peers;
                    return true;
                case 8:
                    providers.privateKeys = occupied.privateKeys;
                    return true;
                case 9:
                    providers.channel = occupied.channel;
                    return true;
                case 1:
                    ++options.trust.revision;
                    return true;
                case 10:
                    options.trust.id = NetworkTrustPolicyId::Create(policy.trust.id.Value() + 1).Value();
                    return true;
                case 11:
                    options.trust.exposure = NetworkExposure::Remote;
                    return true;
                case 12:
                    options.trust.credential = CredentialBindingId::Create(policy.trust.credential.Value() + 1).Value();
                    return true;
                case 13:
                    options.trust.certificate = CertificateBindingId::Create(policy.trust.certificate.Value() + 1).Value();
                    return true;
                case 14:
                    options.trust.privateKey = PrivateKeyBindingId::Create(policy.trust.privateKey.Value() + 1).Value();
                    return true;
                case 15:
                    ++options.trust.maximumProofBytes;
                    return true;
                case 16:
                    options.trust.allowUnprotectedInMemoryLoopback = true;
                    return true;
                default:
                    return false;
            }
        }

        /** @brief Alter one independent resource/factory/canonical-capture prerequisite. */
        void MismatchConfiguration(const unsigned mismatch, NetworkAdmissionPolicy &policy, GnsNativeSecurityOptions &options,
                                   std::shared_ptr<const MessageCodecRegistry> &codecs, const ErrorCodeDescriptor *&expected) {
            switch (mismatch) {
                case 2:
                    ++options.transport.maximumConnections;
                    break;
                case 3:
                    --options.transport.maximumEventsPerPoll;
                    break;
                case 4:
                    --options.transport.maximumMessageBytes;
                    break;
                case 5:
                    options.appId = 0;
                    expected = &NetworkErrors::AuthenticationInvalid;
                    break;
                case 6:
                    codecs.reset();
                    break;
                case 17:
                    // Keep the concrete/local declaration valid; reject only the incompatible envelope ceiling.
                    --policy.envelope.maximumFrameBytes;
                    break;
                default:
                    FAIL("unknown native selection rejection case");
            }
        }
    }  // namespace

    TEST_CASE("Actual product GNS Configure precedes provisioning and preserves startup denial", "[integration][network][gns][security]") {
        ProductPair fixture;
        auto issuer = std::make_shared<ProductRejectingIssuer>();
        auto policy = fixture.Policy(0);
        policy.trust.exposure = NetworkExposure::LocalNetwork;
        policy.trust.allowUnprotectedInMemoryLoopback = false;
        auto providers = fixture.authorities[0]->Providers(fixture.authorities[0]);
        providers.certificates.reset();
        providers.peers.reset();
        providers.privateKeys.reset();
        providers.channel.reset();
        auto selected = Application::Internal::SelectNetworkProductGns(policy, std::move(providers), fixture.codecs, {},
                                                                       DeniedOptions(policy.trust, issuer));
        REQUIRE(selected.HasValue());
        auto composition = std::move(selected).Value();
        const auto capture = composition.admission;
        auto debugger = std::make_shared<Application::NetworkDebuggerService>();
        REQUIRE(debugger->Begin(1901, 1, true, NetworkDiagnosticProvider::Native));
        auto service =
            Application::Internal::CreateNetworkProductTransport(std::move(composition.transport), NetworkProjectRole::DedicatedServer, {},
                                                                 {}, debugger, fixture.owners[0]->sessions, capture);
        REQUIRE(service.HasValue());  // The actual dispatcher/controller configured against the inert declaration.
        REQUIRE(issuer->calls == 0);
        const auto prepared = service.Value()->Prepare();
        REQUIRE(prepared.HasError());
        REQUIRE(prepared.ErrorValue().domain.Value() == NetworkErrors::AuthenticationRejected.domain.Value());
        REQUIRE(prepared.ErrorValue().code.Value() == NetworkErrors::AuthenticationRejected.code.Value());
        REQUIRE(prepared.ErrorValue().message == "product provisioning denied");
        REQUIRE(issuer->calls == 1);
        REQUIRE(issuer->validRequest);
        REQUIRE_FALSE(capture->Providers().certificates->Available());
        service.Value()->Shutdown();
        auto replacement = CreateGnsTransport();
        REQUIRE(replacement.HasValue());
        REQUIRE(replacement.Value()->Initialize({4, 64, 1200}).HasValue());
        replacement.Value()->Shutdown();
    }

    TEST_CASE("The production native selection rejects provider collisions trust and config mismatches before provisioning",
              "[integration][network][gns][security]") {
        const auto mismatch = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17);
        CAPTURE(mismatch);
        ProductPair fixture;
        auto issuer = std::make_shared<ProductRejectingIssuer>();
        auto policy = fixture.Policy(0);
        policy.trust.exposure = NetworkExposure::LocalNetwork;
        policy.trust.allowUnprotectedInMemoryLoopback = false;
        auto options = DeniedOptions(policy.trust, issuer);
        auto providers = fixture.authorities[0]->Providers(fixture.authorities[0]);
        providers.certificates.reset();
        providers.peers.reset();
        providers.privateKeys.reset();
        providers.channel.reset();
        auto codecs = fixture.codecs;
        const auto *expected = &NetworkErrors::AdmissionPolicyInvalid;
        const auto valid = Application::Internal::SelectNetworkProductGns(policy, providers, codecs, {}, options);
        REQUIRE(valid.HasValue());
        REQUIRE(issuer->calls == 0);
        const auto occupied = fixture.authorities[0]->Providers(fixture.authorities[0]);
        if (!MismatchAuthority(mismatch, providers, occupied, policy, options))
            MismatchConfiguration(mismatch, policy, options, codecs, expected);
        const auto selected = Application::Internal::SelectNetworkProductGns(policy, std::move(providers), std::move(codecs), {}, options);
        REQUIRE(selected.HasError());
        REQUIRE(selected.ErrorValue().domain.Value() == expected->domain.Value());
        REQUIRE(selected.ErrorValue().code.Value() == expected->code.Value());
        REQUIRE(issuer->calls == 0);
    }

    TEST_CASE("The production native selection retains one inert declaration and capture without invoking an issuer",
              "[integration][network][gns][security]") {
        ProductPair fixture;
        auto issuer = std::make_shared<ProductRejectingIssuer>();
        auto policy = fixture.Policy(0);
        policy.trust.exposure = NetworkExposure::LocalNetwork;
        policy.trust.allowUnprotectedInMemoryLoopback = false;
        auto providers = fixture.authorities[0]->Providers(fixture.authorities[0]);
        providers.certificates.reset();
        providers.peers.reset();
        providers.privateKeys.reset();
        providers.channel.reset();
        auto selected = Application::Internal::SelectNetworkProductGns(policy, std::move(providers), fixture.codecs, {},
                                                                       DeniedOptions(policy.trust, issuer));
        REQUIRE(selected.HasValue());
        const auto &composition = selected.Value();
        REQUIRE(composition.admission->Policy().local.transport == composition.transport->Capabilities());
        REQUIRE(composition.admission->Policy().trust.id == policy.trust.id);
        REQUIRE(composition.admission->Codecs() == fixture.codecs);
        REQUIRE_FALSE(composition.admission->Providers().certificates->Available());
        REQUIRE_FALSE(composition.admission->Providers().peers->Available());
        REQUIRE_FALSE(composition.admission->Providers().privateKeys->Available());
        REQUIRE(issuer->calls == 0);
        composition.transport->Shutdown();
        REQUIRE_FALSE(composition.admission->Providers().certificates->Available());
    }
}  // namespace Horo::Network::AdmissionTests
