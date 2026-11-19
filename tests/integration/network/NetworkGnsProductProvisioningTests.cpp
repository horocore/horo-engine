#include "GnsTransportFactory.h"
#include "HeadlessInitialStateAuthority.h"
#include "Horo/Network/NetworkAuthoredScenePage.h"
#include "Horo/Network/NetworkInitialStateStream.h"
#include "Horo/Network/NetworkPrefabLifecycle.h"
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

        /** @brief Explicit inert initial-state owner used to check native service composition and retention. */
        class InitialStateProbe final : public INetworkInitialStateHost {
        public:
            Result<NetworkPeerId> ClientRecipient(const PeerSessionLifecycle &) override {
                return Unexpected<NetworkPeerId>();
            }

            Result<std::optional<ReplicationInitialStateSnapshot>> Capture(const ReplicationWorldReadLease &,
                                                                           const ReplicationWorldLifecycle &, const PeerSessionLifecycle &,
                                                                           std::size_t) override {
                return Unexpected<std::optional<ReplicationInitialStateSnapshot>>();
            }

            Result<NetworkInitialStateTarget> PrepareReceiver(const ReplicationInboundAuthority &, ReplicationWorldLifecycle &,
                                                              std::span<const ReplicationInitialOwnership>, std::uint64_t) override {
                return Unexpected<NetworkInitialStateTarget>();
            }

            unsigned calls{};

        private:
            template <typename T> Result<T> Unexpected() {
                ++calls;
                return Result<T>::Failure(MakeError(NetworkErrors::NetworkLifecycleInvalid));
            }
        };

        /** @brief Startup never resolves a gameplay peer before native authentication. */
        class UnusedInitialAuthority final : public Application::Internal::IHeadlessInitialStateAuthority {
        public:
            Result<Application::Internal::HeadlessInitialRecipient> Resolve(const AuthenticationResult &,
                                                                            const ReplicationWorldReadLease &) const override {
                ++calls;
                return Result<Application::Internal::HeadlessInitialRecipient>::Failure(MakeError(NetworkErrors::AuthenticationRejected));
            }

            mutable unsigned calls{};
        };

        /** @brief Extends the same admitted protocol fixture with its real authored, prefab and initial routes. */
        NetworkWorldBindingSetup InitialWorldSetup(const ProductPair &fixture) {
            auto policy = fixture.WorldPolicy();
            const auto protocol = policy.control.protocol;
            const auto route = [protocol](std::uint16_t id) {
                return MessageIdentityDescriptor{protocol,
                                                 TestSupport::WireIdentity<MessageTypeId>(id),
                                                 TestSupport::WireIdentity<MessageSchemaId>(id),
                                                 {1, 0},
                                                 false};
            };
            const std::array routes{route(106), route(107), route(108)};
            policy.authoredScene = NetworkAuthoredScenePolicy{routes[0], policy.feature};
            policy.prefabLifecycle = NetworkPrefabLifecyclePolicy{routes[1], policy.feature};
            policy.initialState = NetworkInitialStatePolicy{routes[2], fixture.features.front(), 4096};
            const std::array capacities{MaximumNetworkAuthoredScenePageBytes, MaximumNetworkPrefabLifecycleBytes,
                                        MaximumNetworkInitialStateFrameBytes};
            std::vector messages(fixture.messages.begin(), fixture.messages.end());
            std::vector codecs(fixture.codecDescriptors.begin(), fixture.codecDescriptors.end());
            for (std::size_t index = 0; index < routes.size(); ++index) {
                messages.push_back(routes[index]);
                codecs.push_back({protocol, routes[index].id, routes[index].schema, {{1, 0}, {1, 0}}, capacities[index]});
            }
            const std::array features{FeatureIdentityDescriptor{protocol, fixture.features[0], {1, 0}},
                                      FeatureIdentityDescriptor{protocol, fixture.features[1], {1, 0}}};
            const auto identities = ProtocolIdentityRegistry::Create({fixture.protocols, messages, features, {}});
            REQUIRE(identities.HasValue());
            auto registry = MessageCodecRegistry::Create({codecs, {}}, identities.Value());
            REQUIRE(registry.HasValue());
            return {policy, std::make_shared<const MessageCodecRegistry>(std::move(registry).Value())};
        }

        /** @brief Explicit startup-denial selection, separate from any successful authentication fixture. */
        GnsNativeSecurityOptions DeniedOptions(const NetworkTrustPolicySnapshot &trust,
                                               const std::shared_ptr<ProductRejectingIssuer> &issuer) {
            GnsNativeSecurityOptions options;
            options.trustedRoot.fill(std::byte{1});  // Explicit public test root; no successful certificate or peer claim.
            options.appId = 480;
            options.trust = trust;
            options.peerTrust = NetworkTrustLevel::Paired;
            options.transport = {4, 64, 1200, 2};
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

    TEST_CASE("Secure product transport preserves the configured initial-state owner and rejects a different trust policy",
              "[integration][network][gns][security][replication]") {
        using namespace Application::Internal;
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
        const auto world = InitialWorldSetup(fixture);
        auto authority = std::make_shared<UnusedInitialAuthority>();
        auto delegate = std::make_shared<InitialStateProbe>();
        auto owner = std::make_shared<HeadlessInitialStateHost>(authority, delegate);
        const std::weak_ptr<HeadlessInitialStateHost> lifetime = owner;
        auto admitted = NetworkProductAdmission::Capture(policy, std::move(providers), world.codecs, {}, world.policy, owner);
        REQUIRE(admitted.HasValue());
        auto admission = std::move(admitted).Value();
        auto debugger = std::make_shared<Application::NetworkDebuggerService>();
        REQUIRE(debugger->Begin(1901, 1, true, NetworkDiagnosticProvider::Native));
        auto options = DeniedOptions(policy.trust, issuer);
        ++options.trust.revision;
        const auto rejected = CreateSecureNetworkProductTransport(NetworkProjectRole::DedicatedServer, {}, {}, debugger,
                                                                  fixture.owners[0]->sessions, admission, options);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == NetworkErrors::AdmissionPolicyInvalid.code.Value());
        CHECK(issuer->calls == 0);
        options.trust = policy.trust;

        auto service = CreateSecureNetworkProductTransport(NetworkProjectRole::DedicatedServer, {}, {}, debugger,
                                                           fixture.owners[0]->sessions, admission, options);
        REQUIRE(service.HasValue());
        admission.reset();
        owner.reset();
        REQUIRE_FALSE(lifetime.expired());
        CHECK(delegate->calls == 0);
        CHECK(authority->calls == 0);
        CHECK(issuer->calls == 0);
        const auto prepared = service.Value()->Prepare();
        REQUIRE(prepared.HasError());
        CHECK(prepared.ErrorValue().code.Value() == NetworkErrors::AuthenticationRejected.code.Value());
        CHECK(issuer->calls == 1);
        CHECK(authority->calls == 0);
        service.Value()->Shutdown();
        CHECK(lifetime.expired());
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
