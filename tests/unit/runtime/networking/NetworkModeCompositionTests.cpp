#include "Horo/Network/NetworkModeComposition.h"
#include "NetworkTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Horo::Network {
    using TestSupport::Bytes;
    using TestSupport::Connection;
    using TestSupport::Id;
    using TestSupport::RequireError;
    using TestSupport::Session;
    using TestSupport::WireIdentity;

    namespace {
        struct Fixture final {
            NetworkTargetAssessment assessment{};
            NetworkTargetSelection selection{};

            explicit Fixture(const NetworkProjectRole role) {
                assessment.diagnostic.reason = NetworkTargetFailureReason::None;
                assessment.matrix.build = Id<NetworkProductBuildId>(10);
                assessment.matrix.productRevision = Id<NetworkProductCapabilityRevision>(11);
                assessment.matrix.hostRevision = Id<NetworkHostCapabilityRevision>(12);
                assessment.matrix.projectRevision = Id<NetworkProjectSettingsRevision>(13);
                assessment.matrix.roles[static_cast<std::size_t>(role)].selected = true;
                selection.role = role;
                selection.expectedBuild = assessment.matrix.build;
                selection.expectedProductRevision = assessment.matrix.productRevision;
                selection.expectedHostRevision = assessment.matrix.hostRevision;
                selection.expectedProjectRevision = assessment.matrix.projectRevision;
                if (role != NetworkProjectRole::Standalone) {
                    selection.provider = Id<NetworkTransportProviderId>(14);
                    selection.protocolVersion = {1, 0};
                }
            }

            [[nodiscard]] Result<NetworkModePlan> Plan(const std::span<const NetworkModeWorld> worlds,
                                                       const NetworkModePresentation presentation = {}) const {
                return ResolveNetworkModePlan(assessment, selection, 17, worlds, presentation);
            }
        };

        [[nodiscard]] NetworkModeWorld World(const NetworkModeWorldKind kind, const std::uint64_t scene,
                                             const std::uint64_t authority = 0) {
            return {kind, Runtime::SceneRuntimeId{scene},
                    authority == 0 ? ReplicationAuthorityEpoch{} : ReplicationAuthorityEpoch::Create(authority).Value()};
        }

        struct Calls final {
            std::vector<NetworkModeServiceRequest> created;
            std::vector<NetworkModeServiceRequest> prepared;
            std::vector<NetworkModeServiceRequest> activated;
            std::vector<NetworkModeServiceRequest> stopped;
            std::vector<Runtime::RuntimePhase> phases;
            NetworkModeServiceKind failPrepare{NetworkModeServiceKind::Count};
            NetworkModeServiceKind failActivate{NetworkModeServiceKind::Count};
        };

        class Service final : public INetworkModeService {
        public:
            Service(std::shared_ptr<Calls> calls, NetworkModeServiceRequest request) : calls_(std::move(calls)), request_(request) {}

            Result<void> Prepare() override {
                calls_->prepared.push_back(request_);
                if (calls_->failPrepare == request_.service)
                    return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable, "Injected prepare failure."));
                return Result<void>::Success();
            }

            Result<void> Activate() override {
                calls_->activated.push_back(request_);
                if (calls_->failActivate == request_.service)
                    return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable, "Injected activate failure."));
                return Result<void>::Success();
            }

            Result<void> RunPhase(const Runtime::RuntimePhase phase) override {
                calls_->phases.push_back(phase);
                return Result<void>::Success();
            }

            Result<void> RunFixedTick(const Runtime::FixedStepContext &) override {
                calls_->phases.push_back(Runtime::RuntimePhase::FixedUpdate);
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                calls_->stopped.push_back(request_);
            }

        private:
            std::shared_ptr<Calls> calls_;
            NetworkModeServiceRequest request_;
        };

        [[nodiscard]] NetworkModeFactories Factories(const std::shared_ptr<Calls> &calls) {
            NetworkModeFactories factories;
            for (auto &factory : factories.services) {
                factory = [calls](const NetworkModeServiceRequest request) -> Result<std::unique_ptr<INetworkModeService>> {
                    calls->created.push_back(request);
                    return Result<std::unique_ptr<INetworkModeService>>::Success(std::make_unique<Service>(calls, request));
                };
            }
            return factories;
        }

        [[nodiscard]] std::size_t Count(const std::vector<NetworkModeServiceRequest> &requests, const NetworkModeServiceKind kind) {
            return static_cast<std::size_t>(std::count_if(requests.begin(), requests.end(), [kind](const auto &request) {
                return request.service == kind;
            }));
        }

        [[nodiscard]] HandshakeSelection Negotiation() {
            HandshakeSelection selection;
            selection.connection = Connection();
            selection.sessionGeneration = Session();
            selection.protocol = WireIdentity<ProtocolId>(1);
            selection.version = {1, 2};
            selection.schemaFingerprint = 42;
            selection.features.values[0] = WireIdentity<ProtocolFeatureId>(1);
            selection.features.values[1] = WireIdentity<ProtocolFeatureId>(2);
            selection.features.count = 2;
            selection.compression = HandshakeCompression::None;
            selection.transport.capabilityRevision = 3;
            selection.transport.admittedDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
            selection.transport.channelCount = 2;
            selection.transport.maximumMessageBytes = 1200;
            return selection;
        }

        [[nodiscard]] AuthenticationResult Authentication() {
            AuthenticationResult result;
            result.connection = Connection();
            result.sessionGeneration = Session();
            result.policy = Id<NetworkTrustPolicyId>(10);
            result.policyRevision = 4;
            result.principal.principal = Id<NetworkPrincipalId>(20);
            result.principal.session.bytes = Bytes<NetworkSessionIdBytes>(0x80);
            result.principal.trustLevel = NetworkTrustLevel::ProductAnchor;
            result.principal.roles.values[0] = Id<NetworkRoleId>(30);
            result.principal.roles.count = 1;
            result.principal.capabilities.values[0] = Id<NetworkCapabilityId>(40);
            result.principal.capabilities.count = 1;
            result.principal.provenance = Id<CredentialProvenanceId>(50);
            result.principal.expiresAtTick = 150;
            result.secureChannel.stamp = {Connection(), Session(), 5};
            result.secureChannel.binding = Id<PrivateKeyBindingId>(11);
            result.secureChannel.channel = Id<SecureChannelId>(12);
            result.secureChannel.channelGeneration = 6;
            result.secureChannel.bindingDigest = Bytes<AuthenticationDigestBytes>(0x20);
            return result;
        }

        [[nodiscard]] PeerSessionLifecycle ActiveSession() {
            auto session = PeerSessionLifecycle::Create(Connection(), Session(), {10, 20, 30, 100, 10}).Value();
            REQUIRE(session.BeginNegotiation(Connection(), Session(), 1).HasValue());
            REQUIRE(session.AcceptNegotiation(Negotiation(), 2).HasValue());
            REQUIRE(session.AcceptAuthentication(Authentication(), 11).HasValue());
            REQUIRE(session.Activate({Connection(), Session(), Id<SecureChannelId>(12), 6, Bytes<AuthenticationDigestBytes>(0x20)}, 21)
                        .HasValue());
            return session;
        }
    }  // namespace

    TEST_CASE("Network mode plan validates exact target evidence, world shape and presentation", "[unit][network][mode]") {
        const std::array standalone{World(NetworkModeWorldKind::Standalone, 1)};
        Fixture fixture(NetworkProjectRole::Standalone);
        REQUIRE(fixture.Plan(standalone).HasValue());
        RequireError(fixture.Plan({}), NetworkErrors::NetworkModeInvalid);
        fixture.assessment.diagnostic.reason = NetworkTargetFailureReason::NotPackaged;
        RequireError(fixture.Plan(standalone), NetworkErrors::NetworkModeInvalid);
        fixture.assessment.diagnostic.reason = NetworkTargetFailureReason::None;
        fixture.selection.expectedHostRevision = Id<NetworkHostCapabilityRevision>(99);
        RequireError(fixture.Plan(standalone), NetworkErrors::NetworkModeInvalid);

        const std::array listen{World(NetworkModeWorldKind::AuthorityServer, 2, 3), World(NetworkModeWorldKind::Client, 4)};
        Fixture listenFixture(NetworkProjectRole::ListenServer);
        RequireError(listenFixture.Plan(listen), NetworkErrors::NetworkModeInvalid);
        REQUIRE(listenFixture.Plan(listen, {.localPlayer = true}).HasValue());
        const std::array duplicate{listen[0], World(NetworkModeWorldKind::Client, 2)};
        RequireError(listenFixture.Plan(duplicate, {.localPlayer = true}), NetworkErrors::NetworkModeInvalid);
        const std::array dedicated{World(NetworkModeWorldKind::AuthorityServer, 5, 6)};
        RequireError(Fixture(NetworkProjectRole::DedicatedServer).Plan(dedicated, {.renderer = true}), NetworkErrors::NetworkModeInvalid);
    }

    TEST_CASE("Standalone omits network factories and dedicated omits presentation without changing Scene and Physics gameplay",
              "[unit][network][mode][headless]") {
        for (const auto mode : {NetworkProjectRole::Standalone, NetworkProjectRole::DedicatedServer}) {
            const std::array world{mode == NetworkProjectRole::Standalone ? World(NetworkModeWorldKind::Standalone, 1)
                                                                          : World(NetworkModeWorldKind::AuthorityServer, 2, 3)};
            auto plan =
                Fixture(mode).Plan(world, mode == NetworkProjectRole::Standalone
                                              ? NetworkModePresentation{.audio = true, .renderer = true, .input = true, .localPlayer = true}
                                              : NetworkModePresentation{});
            REQUIRE(plan.HasValue());
            const auto calls = std::make_shared<Calls>();
            auto composed = NetworkModeComposition::Create(plan.Value(), Factories(calls));
            REQUIRE(composed.HasValue());
            auto host = std::move(composed).Value();
            REQUIRE(host.Start().HasValue());
            REQUIRE(host.Active());
            CHECK(Count(calls->created, NetworkModeServiceKind::Scene) == 1);
            CHECK(Count(calls->created, NetworkModeServiceKind::Physics) == 1);
            CHECK(Count(calls->created, NetworkModeServiceKind::Transport) == (mode == NetworkProjectRole::Standalone ? 0 : 1));
            CHECK(Count(calls->created, NetworkModeServiceKind::Session) == (mode == NetworkProjectRole::Standalone ? 0 : 1));
            CHECK(Count(calls->created, NetworkModeServiceKind::Replication) == (mode == NetworkProjectRole::Standalone ? 0 : 1));
            if (mode == NetworkProjectRole::DedicatedServer) {
                for (const auto omitted : {NetworkModeServiceKind::Audio, NetworkModeServiceKind::Renderer, NetworkModeServiceKind::Gui,
                                           NetworkModeServiceKind::Input, NetworkModeServiceKind::LocalPlayer})
                    CHECK(Count(calls->created, omitted) == 0);
            }
            CancellationSource cancellation;
            REQUIRE(host.RunFixedTick({1, Duration::FromNanoseconds(16'666'667), cancellation.Token()}).HasValue());
            CHECK(calls->phases.size() == calls->created.size());
            REQUIRE(host.Role(world[0].kind, world[0].scene).HasValue());
            host.Shutdown();
            host.Shutdown();
            CHECK(calls->stopped.size() == calls->created.size());
            RequireError(host.Role(world[0].kind, world[0].scene), NetworkErrors::NetworkModeUnavailable);
        }
    }

    TEST_CASE("Mode startup fails atomically and reverses every constructed participant", "[unit][network][mode][failure]") {
        const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 1, 10), World(NetworkModeWorldKind::Client, 2)};
        auto plan = Fixture(NetworkProjectRole::ListenServer).Plan(worlds, {.localPlayer = true});
        REQUIRE(plan.HasValue());
        const auto calls = std::make_shared<Calls>();
        calls->failActivate = NetworkModeServiceKind::Physics;
        auto composed = NetworkModeComposition::Create(plan.Value(), Factories(calls));
        REQUIRE(composed.HasValue());
        auto host = std::move(composed).Value();
        RequireError(host.Start(), NetworkErrors::NetworkModeUnavailable);
        CHECK_FALSE(host.Active());
        CHECK(calls->stopped.size() == calls->created.size());
        RequireError(host.Role(NetworkModeWorldKind::AuthorityServer, worlds[0].scene), NetworkErrors::NetworkModeUnavailable);
    }

    TEST_CASE("Client admission, disconnect and listen travel fence session and Scene generations", "[unit][network][mode][lifecycle]") {
        const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 1, 10), World(NetworkModeWorldKind::Client, 2)};
        auto plan = Fixture(NetworkProjectRole::ListenServer).Plan(worlds, {.localPlayer = true});
        REQUIRE(plan.HasValue());
        const auto calls = std::make_shared<Calls>();
        auto composed = NetworkModeComposition::Create(plan.Value(), Factories(calls));
        REQUIRE(composed.HasValue());
        auto host = std::move(composed).Value();
        REQUIRE(host.Start().HasValue());
        auto pending = PeerSessionLifecycle::Create(Connection(), Session(), {10, 20, 30, 100, 10});
        REQUIRE(pending.HasValue());
        RequireError(host.AdmitClientSession(NetworkModeWorldKind::Client, pending.Value(), Connection(), Session(), 1),
                     NetworkErrors::GameplayDispatchRejected);
        REQUIRE_FALSE(host.Role(NetworkModeWorldKind::Client, worlds[1].scene).Value().session.has_value());
        auto active = ActiveSession();
        REQUIRE(host.AdmitClientSession(NetworkModeWorldKind::Client, active, Connection(), Session(), 22).HasValue());
        CHECK(host.Role(NetworkModeWorldKind::Client, worlds[1].scene).Value().session == Session());
        CHECK(host.Role(NetworkModeWorldKind::AuthorityServer, worlds[0].scene).Value().authority == worlds[0].authority);
        const auto oldClientView = host.Role(NetworkModeWorldKind::Client, worlds[1].scene).Value();
        REQUIRE(host.ValidateRole(oldClientView).HasValue());

        const std::array next{World(NetworkModeWorldKind::AuthorityServer, 3, 11), World(NetworkModeWorldKind::Client, 4)};
        RequireError(host.Travel(next, Runtime::RuntimePhase::FixedUpdate, Session()), NetworkErrors::NetworkModeInvalid);
        RequireError(host.Travel(next, Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Session(9)), NetworkErrors::NetworkModeStale);
        REQUIRE(host.Travel(next, Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Session()).HasValue());
        RequireError(host.Role(NetworkModeWorldKind::Client, worlds[1].scene), NetworkErrors::NetworkModeStale);
        RequireError(host.Role(NetworkModeWorldKind::AuthorityServer, worlds[0].scene), NetworkErrors::NetworkModeStale);
        RequireError(host.ValidateRole(oldClientView), NetworkErrors::NetworkModeStale);
        CHECK(host.Role(NetworkModeWorldKind::AuthorityServer, next[0].scene).Value().authority == next[0].authority);
        CHECK(host.Role(NetworkModeWorldKind::Client, next[1].scene).Value().session == Session());
        const auto beforeDisconnect = host.Role(NetworkModeWorldKind::Client, next[1].scene).Value();
        RequireError(host.DisconnectClient(NetworkModeWorldKind::Client, Session(9)), NetworkErrors::NetworkModeStale);
        REQUIRE(host.DisconnectClient(NetworkModeWorldKind::Client, Session()).HasValue());
        CHECK_FALSE(host.Role(NetworkModeWorldKind::Client, next[1].scene).Value().session.has_value());
        RequireError(host.ValidateRole(beforeDisconnect), NetworkErrors::NetworkModeStale);
        RequireError(host.AdmitClientSession(NetworkModeWorldKind::Client, active, Connection(), Session(), 23),
                     NetworkErrors::NetworkModeStale);
        CHECK(host.Role(NetworkModeWorldKind::AuthorityServer, next[0].scene).Value().authority == next[0].authority);
        host.Shutdown();
        RequireError(host.Travel(next, Runtime::RuntimePhase::CommitDeferredLifecycleChanges), NetworkErrors::NetworkModeUnavailable);
    }

    TEST_CASE("Aggregate listen travel retains both old worlds after candidate failure", "[unit][network][mode][rollback]") {
        const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 21, 30), World(NetworkModeWorldKind::Client, 22)};
        auto plan = Fixture(NetworkProjectRole::ListenServer).Plan(worlds, {.localPlayer = true});
        REQUIRE(plan.HasValue());
        const auto calls = std::make_shared<Calls>();
        auto composed = NetworkModeComposition::Create(plan.Value(), Factories(calls));
        REQUIRE(composed.HasValue());
        auto host = std::move(composed).Value();
        REQUIRE(host.Start().HasValue());
        auto active = ActiveSession();
        REQUIRE(host.AdmitClientSession(NetworkModeWorldKind::Client, active, Connection(), Session(), 22).HasValue());
        calls->failPrepare = NetworkModeServiceKind::Physics;
        const std::array next{World(NetworkModeWorldKind::AuthorityServer, 23, 31), World(NetworkModeWorldKind::Client, 24)};
        RequireError(host.Travel(next, Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Session()),
                     NetworkErrors::NetworkModeUnavailable);
        CHECK(host.Role(NetworkModeWorldKind::AuthorityServer, worlds[0].scene).HasValue());
        CHECK(host.Role(NetworkModeWorldKind::Client, worlds[1].scene).Value().session == Session());
        RequireError(host.Role(NetworkModeWorldKind::AuthorityServer, next[0].scene), NetworkErrors::NetworkModeStale);
        calls->failPrepare = NetworkModeServiceKind::Count;
        REQUIRE(host.Travel(next, Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Session()).HasValue());
        host.Shutdown();
    }
}  // namespace Horo::Network
