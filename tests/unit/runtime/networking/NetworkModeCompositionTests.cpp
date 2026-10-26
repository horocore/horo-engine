#include "Horo/Network/NetworkModeComposition.h"
#include "NetworkModeBindingTestSupport.h"
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

        [[nodiscard]] NetworkModeWorld World(const NetworkModeWorldKind kind, const std::uint64_t scene) {
            return {kind, Runtime::SceneRuntimeId{scene}};
        }

        struct Calls final {
            std::shared_ptr<ModeBindingTests::BindingFixture> binding = std::make_shared<ModeBindingTests::BindingFixture>();
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

        [[nodiscard]] std::shared_ptr<PeerSessionLifecycle> ActiveSession() {
            return ModeBindingTests::Active(Connection(), Session());
        }

        Result<NetworkModeComposition> Compose(NetworkModePlan plan, NetworkModeFactories factories, const std::shared_ptr<Calls> &calls) {
            auto composed =
                NetworkModeComposition::Create(plan, std::move(factories), NetworkWorldCompositionPurpose::Gameplay, calls->binding->setup);
            if (composed.HasError())
                return composed;
            auto mode = std::move(composed).Value();
            if (plan.mode != NetworkProjectRole::Standalone)
                calls->binding->Attach(mode);
            return Result<NetworkModeComposition>::Success(std::move(mode));
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

        const std::array listen{World(NetworkModeWorldKind::AuthorityServer, 2), World(NetworkModeWorldKind::Client, 4)};
        Fixture listenFixture(NetworkProjectRole::ListenServer);
        RequireError(listenFixture.Plan(listen), NetworkErrors::NetworkModeInvalid);
        REQUIRE(listenFixture.Plan(listen, {.localPlayer = true}).HasValue());
        const std::array duplicate{listen[0], World(NetworkModeWorldKind::Client, 2)};
        RequireError(listenFixture.Plan(duplicate, {.localPlayer = true}), NetworkErrors::NetworkModeInvalid);
        const std::array dedicated{World(NetworkModeWorldKind::AuthorityServer, 5)};
        RequireError(Fixture(NetworkProjectRole::DedicatedServer).Plan(dedicated, {.renderer = true}), NetworkErrors::NetworkModeInvalid);
    }

    TEST_CASE("Standalone omits network factories and dedicated omits presentation without changing Scene and Physics gameplay",
              "[unit][network][mode][headless]") {
        for (const auto mode : {NetworkProjectRole::Standalone, NetworkProjectRole::DedicatedServer}) {
            const std::array world{mode == NetworkProjectRole::Standalone ? World(NetworkModeWorldKind::Standalone, 1)
                                                                          : World(NetworkModeWorldKind::AuthorityServer, 2)};
            auto plan =
                Fixture(mode).Plan(world, mode == NetworkProjectRole::Standalone
                                              ? NetworkModePresentation{.audio = true, .renderer = true, .input = true, .localPlayer = true}
                                              : NetworkModePresentation{});
            REQUIRE(plan.HasValue());
            const auto calls = std::make_shared<Calls>();
            auto composed = Compose(plan.Value(), Factories(calls), calls);
            REQUIRE(composed.HasValue());
            auto host = std::move(composed).Value();
            REQUIRE(host.Start().HasValue());
            REQUIRE(host.ServicesStarted());
            CHECK(host.Active() == (mode == NetworkProjectRole::Standalone));
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
            REQUIRE(host.RunPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
            REQUIRE(host.Active());
            calls->phases.clear();
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
        const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 1), World(NetworkModeWorldKind::Client, 2)};
        auto plan = Fixture(NetworkProjectRole::ListenServer).Plan(worlds, {.localPlayer = true});
        REQUIRE(plan.HasValue());
        const auto calls = std::make_shared<Calls>();
        calls->failActivate = NetworkModeServiceKind::Physics;
        auto composed = Compose(plan.Value(), Factories(calls), calls);
        REQUIRE(composed.HasValue());
        auto host = std::move(composed).Value();
        RequireError(host.Start(), NetworkErrors::NetworkModeUnavailable);
        CHECK_FALSE(host.Active());
        CHECK(calls->stopped.size() == calls->created.size());
        RequireError(host.Role(NetworkModeWorldKind::AuthorityServer, worlds[0].scene), NetworkErrors::NetworkModeUnavailable);
    }

    TEST_CASE("Client admission, disconnect and listen travel fence session and Scene generations", "[unit][network][mode][lifecycle]") {
        const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 1), World(NetworkModeWorldKind::Client, 2)};
        auto plan = Fixture(NetworkProjectRole::ListenServer).Plan(worlds, {.localPlayer = true});
        REQUIRE(plan.HasValue());
        const auto calls = std::make_shared<Calls>();
        auto composed = Compose(plan.Value(), Factories(calls), calls);
        REQUIRE(composed.HasValue());
        auto host = std::move(composed).Value();
        REQUIRE(host.Start().HasValue());
        auto pending = PeerSessionLifecycle::Create(Connection(), Session(), {10, 20, 30, 100, 10});
        REQUIRE(pending.HasValue());
        RequireError(host.AdmitClientSession(NetworkModeWorldKind::Client,
                                             std::make_shared<PeerSessionLifecycle>(std::move(pending).Value()), Connection(), Session(),
                                             1),
                     NetworkErrors::GameplayDispatchRejected);
        REQUIRE(host.Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());
        auto active = ActiveSession();
        REQUIRE(host.AdmitClientSession(NetworkModeWorldKind::Client, active, Connection(), Session(), 22).HasValue());
        calls->binding->PublishInitial(host, active);
        CHECK(host.Role(NetworkModeWorldKind::Client, worlds[1].scene).Value().session == Session());
        CHECK(host.Role(NetworkModeWorldKind::AuthorityServer, worlds[0].scene).Value().authority.IsValid());
        const auto oldClientView = host.Role(NetworkModeWorldKind::Client, worlds[1].scene).Value();
        REQUIRE(host.ValidateRole(oldClientView).HasValue());

        const std::array next{World(NetworkModeWorldKind::AuthorityServer, 3), World(NetworkModeWorldKind::Client, 4)};
        RequireError(host.Travel(next, Runtime::RuntimePhase::FixedUpdate, Session()), NetworkErrors::NetworkModeInvalid);
        RequireError(host.Travel(next, Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Session(9)), NetworkErrors::NetworkModeStale);
        REQUIRE(host.Travel(next, Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Session()).HasValue());
        calls->binding->CommitTravel(host);
        RequireError(host.Role(NetworkModeWorldKind::Client, worlds[1].scene), NetworkErrors::NetworkModeStale);
        RequireError(host.Role(NetworkModeWorldKind::AuthorityServer, worlds[0].scene), NetworkErrors::NetworkModeStale);
        RequireError(host.ValidateRole(oldClientView), NetworkErrors::NetworkModeStale);
        CHECK(host.Role(NetworkModeWorldKind::AuthorityServer, next[0].scene).Value().authority.IsValid());
        CHECK(host.Role(NetworkModeWorldKind::Client, next[1].scene).Value().session == Session());
        const auto beforeDisconnect = host.Role(NetworkModeWorldKind::Client, next[1].scene).Value();
        RequireError(host.DisconnectClient(NetworkModeWorldKind::Client, Session(9)), NetworkErrors::NetworkModeStale);
        REQUIRE(host.DisconnectClient(NetworkModeWorldKind::Client, Session()).HasValue());
        CHECK(host.Role(NetworkModeWorldKind::Client, next[1].scene).HasError());
        // A copied generation is stale even though the disconnected world has no current role to dereference.
        RequireError(host.ValidateRole(beforeDisconnect), NetworkErrors::NetworkModeStale);
        RequireError(host.AdmitClientSession(NetworkModeWorldKind::Client, active, Connection(), Session(), 23),
                     NetworkErrors::NetworkModeStale);
        CHECK(host.Role(NetworkModeWorldKind::AuthorityServer, next[0].scene).Value().authority.IsValid());
        host.Shutdown();
        RequireError(host.Travel(next, Runtime::RuntimePhase::CommitDeferredLifecycleChanges), NetworkErrors::NetworkModeUnavailable);
    }

    TEST_CASE("Aggregate listen travel retains both old worlds after candidate failure", "[unit][network][mode][rollback]") {
        const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 21), World(NetworkModeWorldKind::Client, 22)};
        auto plan = Fixture(NetworkProjectRole::ListenServer).Plan(worlds, {.localPlayer = true});
        REQUIRE(plan.HasValue());
        const auto calls = std::make_shared<Calls>();
        auto composed = Compose(plan.Value(), Factories(calls), calls);
        REQUIRE(composed.HasValue());
        auto host = std::move(composed).Value();
        REQUIRE(host.Start().HasValue());
        auto active = ActiveSession();
        REQUIRE(host.AdmitClientSession(NetworkModeWorldKind::Client, active, Connection(), Session(), 22).HasValue());
        calls->binding->PublishInitial(host, active);
        calls->failPrepare = NetworkModeServiceKind::Physics;
        const std::array next{World(NetworkModeWorldKind::AuthorityServer, 23), World(NetworkModeWorldKind::Client, 24)};
        RequireError(host.Travel(next, Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Session()),
                     NetworkErrors::NetworkModeUnavailable);
        CHECK(host.Role(NetworkModeWorldKind::AuthorityServer, worlds[0].scene).HasValue());
        CHECK(host.Role(NetworkModeWorldKind::Client, worlds[1].scene).Value().session == Session());
        RequireError(host.Role(NetworkModeWorldKind::AuthorityServer, next[0].scene), NetworkErrors::NetworkModeStale);
        calls->failPrepare = NetworkModeServiceKind::Count;
        REQUIRE(host.Travel(next, Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Session()).HasValue());
        calls->binding->CommitTravel(host);
        host.Shutdown();
    }

    TEST_CASE("Canonical world ownership survives mode moves and revokes queued callbacks on shutdown",
              "[unit][network][mode][binding][move]") {
        const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 2), World(NetworkModeWorldKind::Client, 4)};
        const auto plan = Fixture(NetworkProjectRole::ListenServer).Plan(worlds, {.localPlayer = true});
        REQUIRE(plan.HasValue());
        auto calls = std::make_shared<Calls>();
        auto created = Compose(plan.Value(), Factories(calls), calls);
        REQUIRE(created.HasValue());
        auto beforeStart = std::move(created).Value();
        auto stable = NetworkModeHostAccess::Binding(beforeStart).lock();
        REQUIRE(stable);
        REQUIRE(beforeStart.Start().HasValue());
        auto client = ActiveSession();
        REQUIRE(beforeStart.AdmitClientSession(NetworkModeWorldKind::Client, client, Connection(), Session(), 22).HasValue());
        calls->binding->PublishInitial(beforeStart, client);
        auto afterStart = std::move(beforeStart);
        beforeStart.Shutdown();
        CHECK(NetworkModeHostAccess::Binding(afterStart).lock() == stable);
        const auto role = afterStart.Role(NetworkModeWorldKind::Client, worlds[1].scene);
        REQUIRE(role.HasValue());
        auto lease = afterStart.AcquireReplication(NetworkModeWorldKind::Client,
                                                   {worlds[1].scene, *role.Value().worldSession, Runtime::RuntimePhase::NetworkPoll, 0, {}},
                                                   ReplicationWorldCapability::ApplyAuthoritativeState);
        REQUIRE(lease.HasValue());
        calls->binding->transport.event = {.kind = NetworkTransportEventKind::PacketReceived,
                                           .connection = Connection(),
                                           .channel = {},
                                           .delivery = DeliveryPolicy::ReliableOrdered,
                                           .payload = std::vector<std::byte>(calls->binding->transport.sent.begin(),
                                                                             calls->binding->transport.sent.begin() +
                                                                                 calls->binding->transport.size)};
        calls->binding->transport.queued = true;
        afterStart.Shutdown();
        CHECK(lease.Value().IsRevoked());
        CHECK(afterStart.Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());
        const auto polled = calls->binding->dispatcher->RunNetworkPoll(22);
        REQUIRE(polled.HasValue());
        CHECK(polled.Value().rejectedPackets == 1);
        CHECK_FALSE(stable->Ready(NetworkModeWorldKind::Client, worlds[1].scene));
    }

    TEST_CASE("Published listen travel retains exact COMMIT debt through native backpressure", "[unit][network][mode][binding][travel]") {
        const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 2), World(NetworkModeWorldKind::Client, 4)};
        const auto plan = Fixture(NetworkProjectRole::ListenServer).Plan(worlds, {.localPlayer = true});
        REQUIRE(plan.HasValue());
        auto calls = std::make_shared<Calls>();
        auto created = Compose(plan.Value(), Factories(calls), calls);
        REQUIRE(created.HasValue());
        auto mode = std::move(created).Value();
        REQUIRE(mode.Start().HasValue());
        auto client = ActiveSession();
        REQUIRE(mode.AdmitClientSession(NetworkModeWorldKind::Client, client, Connection(), Session(), 22).HasValue());
        calls->binding->PublishInitial(mode, client);
        const auto oldClient = mode.Role(NetworkModeWorldKind::Client, worlds[1].scene);
        REQUIRE(oldClient.HasValue());
        const std::array next{World(NetworkModeWorldKind::AuthorityServer, 3), World(NetworkModeWorldKind::Client, 5)};
        REQUIRE(mode.Travel(next, Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Session()).HasValue());
        calls->binding->Deliver();  // PREPARE is accepted and staged, but old roles still publish.
        CHECK(mode.ValidateRole(oldClient.Value()).HasValue());
        calls->binding->transport.backpressure = true;
        REQUIRE(mode.RunPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
        REQUIRE(mode.Role(NetworkModeWorldKind::Client, next[1].scene).HasValue());
        CHECK(mode.ValidateRole(oldClient.Value()).HasError());
        auto owner = calls->binding->binding.lock();
        REQUIRE(owner);
        REQUIRE(owner->Flush(23).HasValue());
        REQUIRE(owner->CancelTravel().HasValue());  // Cannot discard a published COMMIT.
        REQUIRE(owner->Flush(24).HasValue());
        calls->binding->transport.backpressure = false;
        REQUIRE(owner->Flush(25).HasValue());
        const auto envelope = DecodeMessageEnvelope(std::span{calls->binding->transport.sent}.first(calls->binding->transport.size),
                                                    *calls->binding->setup.codecs);
        REQUIRE(envelope.HasValue());
        CHECK(envelope.Value().sequence.Value() == 3);
        const auto commit = DecodeNetworkWorldBinding(envelope.Value().payload);
        REQUIRE(commit.HasValue());
        CHECK(commit.Value().kind == NetworkWorldBindingKind::CommitTravel);
        CHECK(commit.Value().worldSession == *mode.Role(NetworkModeWorldKind::Client, next[1].scene).Value().worldSession);
    }

    TEST_CASE("Requested network gameplay cannot silently become transport qualification", "[unit][network][mode][binding][policy]") {
        for (const auto selected : {NetworkProjectRole::Client, NetworkProjectRole::ListenServer, NetworkProjectRole::DedicatedServer}) {
            std::array<NetworkModeWorld, 2> worlds{World(NetworkModeWorldKind::AuthorityServer, 1), World(NetworkModeWorldKind::Client, 2)};
            const auto count = selected == NetworkProjectRole::ListenServer ? 2u : 1u;
            if (selected == NetworkProjectRole::Client)
                worlds[0] = worlds[1];
            const auto plan =
                Fixture(selected).Plan(std::span{worlds}.first(count), {.localPlayer = selected != NetworkProjectRole::DedicatedServer});
            REQUIRE(plan.HasValue());
            auto calls = std::make_shared<Calls>();
            const auto missing = NetworkModeComposition::Create(plan.Value(), Factories(calls));
            RequireError(missing, NetworkErrors::AdmissionPolicyInvalid);
            CHECK(calls->created.empty());
            auto qualification =
                NetworkModeComposition::Create(plan.Value(), Factories(calls), NetworkWorldCompositionPurpose::TransportQualification);
            REQUIRE(qualification.HasValue());
            auto transportOnly = std::move(qualification).Value();
            REQUIRE(transportOnly.Start().HasValue());
            REQUIRE(transportOnly.ServicesStarted());
            CHECK(transportOnly.Role(worlds[0].kind, worlds[0].scene).HasError());
            CHECK(NetworkModeHostAccess::Binding(transportOnly).expired());
        }
    }
}  // namespace Horo::Network
