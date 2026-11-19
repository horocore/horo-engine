#include "../NetworkAdmissionProductTestSupport.h"
#include "GnsEvidenceAssertions.h"
#include "GnsLiveSceneSupport.h"
#include "GnsObservedTransport.h"
#include "GnsQualificationSupport.h"
#include "Horo/Foundation/Platform.h"
#include "NetworkProductGnsSelection.h"

#include <algorithm>
#include <catch2/catch_session.hpp>
#include <catch2/internal/catch_clara.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <utility>

namespace Horo::Network::NativeQualification {
    namespace {
        struct ChildInput final {
            std::string directory;
            std::string nativeDirectory;
            bool rejectNativeRoot{};
            bool liveScene{};
            unsigned side{};
        } input;

        std::string publicError;

        void ReportError(const Error &error) {
            publicError = "NATIVE_ERROR " + error.domain.Value() + ':' + error.code.Value();
            std::cout << publicError << '\n' << std::flush;
        }

        /** @brief Handler only accepts the runtime dispatcher's own active session, never a fixture projection. */
        class NativeHandler final : public IInboundMessageHandler {
        public:
            Result<void> Handle(const MessageEnvelope &) override {
                return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
            }

            Result<void> HandleAdmitted(const InboundMessageContext &context, const MessageEnvelope &message) override {
                if (!context.session || context.session->State() != PeerSessionState::Active ||
                    context.session->AdmitGameplay(context.connection, context.generation, context.ownerTick).HasError() ||
                    message.payload != std::vector<std::byte>{expected} || !observation || !observation->transport)
                    return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
                REQUIRE(host != nullptr);
                const auto role = host->Role(world, scene);
                REQUIRE(role.HasValue());
                REQUIRE(role.Value().worldSession.has_value());
                const auto capability = world == NetworkModeWorldKind::AuthorityServer
                                            ? ReplicationWorldCapability::PublishAuthority
                                            : ReplicationWorldCapability::ApplyAuthoritativeState;
                auto lease = host->AcquireReplication(world,
                                                      {scene, *role.Value().worldSession, Runtime::RuntimePhase::NetworkPoll, 0,
                                                       context.cancellation},
                                                      capability);
                REQUIRE(lease.HasValue());
                REQUIRE_FALSE(lease.Value().IsRevoked());
                REQUIRE(lease.Value().Descriptor().session == *role.Value().worldSession);
                if (calls == 0) {
                    REQUIRE(admission != nullptr);
                    REQUIRE(credentials != nullptr);
                    REQUIRE(credentials->grant.has_value());
                    VerifyNativeEvidence(context, *admission, *credentials->grant);
                }
                ++calls;
                if (reply.empty())
                    return Result<void>::Success();
                return observation->transport->Send(context.connection, ChannelId::Create(1, 2).Value(), reply,
                                                    DeliveryPolicy::ReliableOrdered);
            }

            std::shared_ptr<const Application::Internal::NetworkProductAdmission> admission;
            std::shared_ptr<CredentialObservation> credentials;
            std::shared_ptr<TransportObservation> observation;  // Revoked by the exact adapter before shutdown/destruction.
            Application::Internal::NetworkProductHost *host{};  // Synchronous handler borrow, revoked before host teardown.
            NetworkModeWorldKind world{NetworkModeWorldKind::Count};
            Runtime::SceneRuntimeId scene;
            std::byte expected{};
            std::vector<std::byte> reply;
            std::size_t calls{};
        };

        /** @brief Report only fixed source-line identity, never assertion values or private provider material. */
        class FailureLocationListener final : public Catch::EventListenerBase {
        public:
            using EventListenerBase::EventListenerBase;

            void assertionEnded(const Catch::AssertionStats &stats) override {
                if (!stats.assertionResult.isOk()) {
                    // Catch captures test output; listeners run outside that capture boundary.
                    std::cout << "NATIVE_ASSERT " << stats.assertionResult.getSourceInfo().line;
                    if (!publicError.empty())
                        std::cout << ' ' << std::exchange(publicError, {});
                    std::cout << '\n' << std::flush;
                }
            }
        };

        struct ChildOwners final {
            AdmissionTests::ProductPair fixtures;  // Reuse inert inventory and actual headless worlds only.
            std::shared_ptr<SteadyClock> clock = std::make_shared<SteadyClock>();
            std::shared_ptr<NativeHandler> handler = std::make_shared<NativeHandler>();
            std::shared_ptr<TransportObservation> observation = std::make_shared<TransportObservation>();
            std::shared_ptr<CredentialObservation> credentials = std::make_shared<CredentialObservation>();
            std::unique_ptr<INetworkTransport> native;
            std::vector<std::byte> command;
            std::unique_ptr<LiveScene::Product> live;
            std::unique_ptr<Application::Internal::NetworkProductHost> host;

            ChildOwners() {
                // No PairedTransport or fixture certificate/channel/credential authority participates.
                fixtures.authorities = {};
                fixtures.key.reset();
                fixtures.link.reset();
            }
        };

        std::vector<std::byte> Frame(const AdmissionTests::ProductPair &fixture, const std::byte payload) {
            MessageEnvelope message{.protocol = fixture.protocols[0].id,
                                    .message = fixture.messages[4].id,
                                    .schema = fixture.messages[4].schema,
                                    .schemaVersion = {1, 0},
                                    .sequence = MessageSequenceNumber{1},
                                    .payload = {payload}};
            auto encoded = EncodeMessageEnvelope(message, *fixture.codecs, {1200, 1168, 1, 1});
            REQUIRE(encoded.HasValue());
            return std::move(encoded).Value();
        }

        NetworkAdmissionPolicy Policy(ChildOwners &owners) {
            auto policy = owners.fixtures.Policy(input.side);
            policy.trust.exposure = NetworkExposure::Remote;
            policy.trust.allowUnprotectedInMemoryLoopback = false;
            // One explicit millisecond clock domain shared by admission and the actual frame poll.
            policy.timeouts = {3000, 5000, 7000, 30000, 15000};
            if (owners.live)
                owners.live->routes.Apply(policy);
            return policy;
        }

        std::shared_ptr<const Application::Internal::NetworkProductAdmission> Capture(ChildOwners &owners, NetworkAdmissionPolicy &policy) {
            auto credentials = LoadCredentialProviders(input.directory, input.side);
            REQUIRE(credentials.HasValue());
            auto providers = std::move(credentials).Value();
            REQUIRE_FALSE(providers.certificates);
            REQUIRE_FALSE(providers.peers);
            REQUIRE_FALSE(providers.privateKeys);
            REQUIRE_FALSE(providers.channel);
            REQUIRE(providers.credentials != nullptr);
            providers.credentials = std::make_shared<ObservedCredential>(providers.credentials, owners.credentials);
            owners.handler->credentials = owners.credentials;
            auto optionsResult =
                LoadNativeSelection(input.nativeDirectory.empty() ? input.directory : input.nativeDirectory, input.side, policy.trust);
            REQUIRE(optionsResult.HasValue());
            auto options = std::move(optionsResult).Value();
            options.transport.maximumChannels = policy.local.transport.maximumChannels;
            owners.handler->observation = owners.observation;
            owners.handler->expected = input.side == 0 ? std::byte{0x41} : std::byte{0x42};
            if (input.side == 0)
                owners.handler->reply = Frame(owners.fixtures, std::byte{0x42});
            owners.command = Frame(owners.fixtures, std::byte{0x41});
            const std::array bindings{
                Application::Internal::NetworkProductInboundHandler{{owners.fixtures.protocols[0].id, owners.fixtures.messages[4].id,
                                                                     MessageTrafficClass::Command, Runtime::RuntimePhase::NetworkPoll, 64},
                                                                    owners.handler}};
            auto selected =
                Application::Internal::SelectNetworkProductGns(policy, std::move(providers),
                                                               owners.live ? owners.live->routes.codecs : owners.fixtures.codecs, bindings,
                                                               options,
                                                               owners.live ? owners.live->routes.binding : owners.fixtures.WorldPolicy(),
                                                               owners.live ? owners.live->state : nullptr,
                                                               owners.live ? owners.live->state : nullptr);
            REQUIRE(selected.HasValue());
            auto composition = std::move(selected).Value();
            owners.handler->admission = composition.admission;
            policy = composition.admission->Policy();
            if (owners.live)
                owners.observation->snapshotChannel = owners.live->routes.binding.stateUpdates->channel;
            // Passive observation wraps the same selected production candidate; it grants no authority.
            owners.native = std::make_unique<ObservedTransport>(std::move(composition.transport), owners.observation);
            return std::move(composition.admission);
        }

        /** @brief Start the actual selected host and bind its counting handler to the host-owned local world. */
        void StartHost(ChildOwners &owners, const NetworkModeWorld &world) {
            owners.handler->host = owners.host.get();
            owners.handler->world = world.kind;
            owners.handler->scene = world.scene;
            const auto started = owners.host->Startup();
            if (started.HasError())
                ReportError(started.ErrorValue());
            REQUIRE(started.HasValue());
            std::cout << "NATIVE_PHASE started\n" << std::flush;
        }

        void CreateHost(ChildOwners &owners) {
            auto policy = Policy(owners);
            auto capture = Capture(owners, policy);
            auto selectedClock =
                std::make_shared<Application::Internal::NetworkProductSessionClock>(owners.clock, Duration::FromNanoseconds(1000000),
                                                                                    owners.clock->MonotonicNow());
            auto sessions = std::make_shared<Application::Internal::ClientSessionOwner>(selectedClock);
            owners.fixtures.owners[input.side]->sessions = sessions;
            const auto role = input.side == 0 ? NetworkProjectRole::DedicatedServer : NetworkProjectRole::Client;
            const auto &project = owners.live ? *owners.live->project : *owners.fixtures.project;
            auto facts = ProductFacts(project, role, false);
            facts.product.providers[0].capabilities = policy.local.transport;
            facts.inventory.providers[0] = facts.product.providers[0];
            facts.host.providers[0].capabilities = policy.local.transport;
            auto debugger = std::make_shared<Application::NetworkDebuggerService>();
            REQUIRE(debugger->Begin(2201 + input.side, 1, true, NetworkDiagnosticProvider::Native));
            auto address = NetworkAddress::Parse("127.0.0.1:48657");
            REQUIRE(address.HasValue());
            auto factories = owners.fixtures.worlds;
            auto slot = std::make_shared<std::unique_ptr<INetworkTransport>>(std::move(owners.native));
            factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Transport)] =
                [slot, role, endpoint = address.Value(), debugger, sessions, capture](const NetworkModeServiceRequest &) {
                return Application::Internal::CreateNetworkProductTransport(std::move(*slot), role, endpoint, endpoint, debugger, sessions,
                                                                            capture);
            };
            Application::Internal::NetworkProductComposition composition{std::move(factories)};
            if (owners.live) {
                auto configured = owners.live->Compose(std::move(composition.factories));
                if (configured.HasError())
                    ReportError(configured.ErrorValue());
                REQUIRE(configured.HasValue());
                composition = std::move(configured).Value();
                auto &sceneFactory = composition.factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Scene)];
                sceneFactory = [factory = std::move(sceneFactory), &live = *owners.live](const NetworkModeServiceRequest &request) {
                    auto candidate = factory(request);
                    if (candidate.HasValue())
                        live.scene = candidate.Value().get();
                    return candidate;
                };
            }
            const std::array world{input.side == 0 ? World(NetworkModeWorldKind::AuthorityServer, 1701)
                                                   : World(NetworkModeWorldKind::Client, 1702)};
            NetworkTargetDiagnostic targetDiagnostic;
            auto created = Application::Internal::NetworkProductHost::Create(*owners.clock,
                                                                             {project,
                                                                              facts.product,
                                                                              facts.inventory,
                                                                              facts.host,
                                                                              facts.requirements,
                                                                              facts.selection,
                                                                              100 + input.side,
                                                                              world,
                                                                              {.localPlayer = input.side == 1},
                                                                              sessions,
                                                                              NetworkWorldCompositionPurpose::Gameplay,
                                                                              capture->WorldBinding(),
                                                                              composition.fixedParticipant,
                                                                              composition.pollRetirement},
                                                                             std::move(composition.factories), &targetDiagnostic);
            if (created.HasError()) {
                ReportError(created.ErrorValue());
                publicError += " target=" + std::to_string(static_cast<unsigned>(targetDiagnostic.capability)) + ':' +
                               std::to_string(static_cast<unsigned>(targetDiagnostic.reason));
            }
            REQUIRE(created.HasValue());
            owners.host = std::move(created).Value();
            StartHost(owners, world[0]);
        }

        /** @brief Fixed public event kinds, counters and distinct original/canonical error identities only; never native text or payload.
         */
        void ReportPeerState(const ChildOwners &owners, const bool sent) {
            const auto *terminal = owners.host->ClientSessionTerminal();
            if (terminal && terminal->failure) {
                const auto error = terminal->failure->ToError();
                std::cout << "NATIVE_CODE canonical " << error.domain.Value() << ':' << error.code.Value() << '\n';
            }
            if (const auto *failure = owners.observation->failure)
                std::cout << "NATIVE_CODE original " << failure->domain.Value() << ':' << failure->code.Value() << '\n';
            std::cout << "NATIVE_EVENTS";
            for (std::size_t i = 0; i < std::min(owners.observation->events, owners.observation->eventPrefix.size()); ++i)
                std::cout << ':' << static_cast<unsigned>(owners.observation->eventPrefix[i]);
            std::cout << '\n';
            const auto counters = owners.observation->transport ? owners.observation->transport->Stats() : owners.observation->retiredStats;
            std::cout << "NATIVE_STATE " << sent << ':' << owners.observation->outbound.IsValid() << ':' << owners.observation->events
                      << ':' << static_cast<unsigned>(owners.observation->lastEvent) << ':' << counters.activeConnections << ':'
                      << counters.sentMessages << ':' << counters.receivedMessages << ':'
                      << (terminal ? static_cast<unsigned>(terminal->kind) : static_cast<unsigned>(PeerSessionTerminalKind::Count)) << '\n'
                      << std::flush;
            if (owners.live) {
                const auto y = owners.live->SceneY();
                const auto authoredY = input.side == 0 ? LiveScene::AuthorityStartY : LiveScene::ClientStartY;
                std::cout << "NATIVE_STATE scene:" << owners.observation->snapshotsSent << ':' << owners.observation->snapshotsReceived
                          << ':' << y.has_value() << ':' << (y && *y != authoredY) << '\n'
                          << std::flush;
            }
        }

        void ShutdownAndVerifyReuse(ChildOwners &owners) {
            owners.handler->observation.reset();
            if (owners.live)
                owners.live->scene = nullptr;
            owners.host->Shutdown();
            REQUIRE(owners.observation->transport == nullptr);
            owners.host.reset();
            auto reusable = CreateGnsTransport();
            REQUIRE(reusable.HasValue());
            REQUIRE(reusable.Value()->Initialize({4, 64, 1200}).HasValue());
            reusable.Value()->Shutdown();
        }

        const Application::Internal::ClientConnectionAttemptTerminal *RequireRejectedClient(ChildOwners &owners,
                                                                                            const ConnectionHandle attempted,
                                                                                            const bool frameFailed) {
            if (!frameFailed)
                ReportPeerState(owners, false);
            REQUIRE(frameFailed);
            const auto *terminal = owners.host->ClientConnectionTerminal();
            REQUIRE(terminal);
            REQUIRE(terminal->connection == attempted);
            REQUIRE(terminal->failure);
            REQUIRE(terminal->failure->domain.Value() == NetworkErrors::TransportConnectionFailed.domain.Value());
            REQUIRE(terminal->failure->code.Value() == NetworkErrors::TransportConnectionFailed.code.Value());
            REQUIRE(owners.observation->failure == terminal->failure);
            REQUIRE(owners.host->ClientSessionTerminal() == nullptr);
            REQUIRE(owners.host->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702}).HasError());
            REQUIRE(owners.observation->transport == nullptr);
            return terminal;
        }

        /** @brief Distinct native roots must reject before transport readiness or either canonical gameplay authority can publish. */
        void PumpRejectedRoot(ChildOwners &owners) {
            if (input.side == 0)
                std::cout << "NATIVE_READY server\n" << std::flush;
            const auto attempted = owners.observation->outbound;
            // The other real process performs cold image verification before it can connect.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            bool frameFailed{};
            while (std::chrono::steady_clock::now() < deadline) {
                const auto frame = owners.host->RunFrame();
                if (frame.HasError()) {
                    ReportError(frame.ErrorValue());
                    REQUIRE(input.side == 1);
                    REQUIRE(frame.ErrorValue().domain.Value() == NetworkErrors::TransportConnectionFailed.domain.Value());
                    REQUIRE(frame.ErrorValue().code.Value() == NetworkErrors::TransportConnectionFailed.code.Value());
                    frameFailed = true;
                    break;
                }
                REQUIRE(owners.observation->readyEvents == 0);
                REQUIRE(owners.handler->calls == 0);
                if (input.side == 0 && std::filesystem::exists(std::filesystem::path(input.directory) / "stop-server"))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const auto stats = owners.observation->transport ? owners.observation->transport->Stats() : owners.observation->retiredStats;
            REQUIRE(stats.activeConnections == 0);
            REQUIRE(stats.sentMessages == 0);
            REQUIRE(stats.receivedMessages == 0);
            const auto *terminal = input.side == 1 ? RequireRejectedClient(owners, attempted, frameFailed) : nullptr;
            REQUIRE(owners.observation->readyEvents == 0);
            REQUIRE(owners.handler->calls == 0);
            ShutdownAndVerifyReuse(owners);
            if (input.side == 1)
                REQUIRE(owners.fixtures.owners[1]->sessions->ConnectionAttemptTerminal() == terminal);
            std::cout << "NATIVE_REJECTED " << (input.side == 0 ? "server" : "client") << '\n' << std::flush;
        }

        void Pump(ChildOwners &owners) {
            if (input.side == 0) {
                std::cout << "NATIVE_READY server\n" << std::flush;
            }
            // Covers peer cold startup; the canonical admission policy still owns its strict protocol timeouts.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            bool sent{};
            std::optional<float> authorityY;
            std::size_t snapshotsAtFirstValue{};
            bool appliedSnapshot{};
            while (std::chrono::steady_clock::now() < deadline) {
                auto frame = owners.host->RunFrame();
                if (frame.HasError())
                    ReportError(frame.ErrorValue());
                REQUIRE(frame.HasValue());
                if (input.side == 1 && owners.live) {
                    const auto y = owners.live->SceneY();
                    if (y && *y != LiveScene::ClientStartY) {
                        const auto snapshots = owners.observation->snapshotsReceived;
                        // The initial receipt and first live packet can be processed in the same host frame.
                        if (!authorityY) {
                            authorityY = y;
                            snapshotsAtFirstValue = snapshots;
                        } else if (snapshots > snapshotsAtFirstValue && *y < *authorityY) {
                            appliedSnapshot = true;
                        }
                    }
                }
                if (input.side == 1 && !sent && (!owners.live || appliedSnapshot)) {
                    const auto role = owners.host->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702});
                    if (role.HasValue() && role.Value().session.has_value()) {
                        REQUIRE(role.Value().worldSession.has_value());
                        REQUIRE_FALSE(role.Value().authority.IsValid());
                        REQUIRE(owners.observation->outbound.IsValid());
                        REQUIRE(owners.observation->transport
                                    ->Send(owners.observation->outbound, ChannelId::Create(1, 2).Value(), owners.command,
                                           DeliveryPolicy::ReliableOrdered)
                                    .HasValue());
                        sent = true;
                    }
                }
                if ((input.side == 1 && owners.handler->calls > 0) ||
                    (input.side == 0 && std::filesystem::exists(std::filesystem::path(input.directory) / "stop-server")))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));  // Qualification loop only, outside engine polling callbacks.
            }
            if (owners.handler->calls == 0)
                ReportPeerState(owners, sent);
            REQUIRE(owners.handler->calls > 0);
            if (owners.live) {
                if (input.side == 0) {
                    const auto y = owners.live->SceneY();
                    REQUIRE(y);
                    REQUIRE(*y < LiveScene::AuthorityStartY);
                    REQUIRE(owners.observation->snapshotsSent > 0);
                } else {
                    REQUIRE(authorityY);
                    REQUIRE(appliedSnapshot);
                    REQUIRE(owners.observation->snapshotsReceived > 0);
                }
                std::cout << "NATIVE_SNAPSHOT " << (input.side == 0 ? "server" : "client") << '\n' << std::flush;
            }
            REQUIRE(owners.host->ClientConnectionTerminal() == nullptr);
            ShutdownAndVerifyReuse(owners);
            REQUIRE(owners.fixtures.owners[input.side]->sessions->ConnectionAttemptTerminal() == nullptr);
            std::cout << "NATIVE_QUALIFIED " << (input.side == 0 ? "server" : "client") << '\n' << std::flush;
        }
    }  // namespace

    TEST_CASE("Real native product child authenticates its own peer and dispatches registered gameplay", "[gns][native-child]") {
        REQUIRE(input.side <= 1);
        REQUIRE_FALSE(input.directory.empty());
        ChildOwners owners;
        if (input.liveScene) {
            REQUIRE_FALSE(input.rejectNativeRoot);
            owners.live = std::make_unique<LiveScene::Product>(owners.fixtures, owners.credentials, input.side == 0);
        }
        CreateHost(owners);
        if (input.rejectNativeRoot)
            PumpRejectedRoot(owners);
        else
            Pump(owners);
    }
}  // namespace Horo::Network::NativeQualification

CATCH_REGISTER_LISTENER(Horo::Network::NativeQualification::FailureLocationListener)

int main(int argc, char **argv) {
    Catch::Session session;
    auto cli = session.cli() | Catch::Clara::Opt(Horo::Network::NativeQualification::input.directory, "directory")["--fixture-dir"] |
               Catch::Clara::Opt(Horo::Network::NativeQualification::input.side, "side")["--side"] |
               Catch::Clara::Opt(Horo::Network::NativeQualification::input.nativeDirectory, "native-directory")["--native-dir"] |
               Catch::Clara::Opt(Horo::Network::NativeQualification::input.rejectNativeRoot)["--reject-native-root"] |
               Catch::Clara::Opt(Horo::Network::NativeQualification::input.liveScene)["--live-scene"];
    session.cli(cli);
    return session.run(argc, argv);
}
