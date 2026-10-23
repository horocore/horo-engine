#include "../NetworkAdmissionProductTestSupport.h"
#include "GnsObservedTransport.h"
#include "GnsQualificationSupport.h"
#include "Horo/Foundation/Platform.h"

#include <algorithm>
#include <catch2/catch_session.hpp>
#include <catch2/internal/catch_clara.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace Horo::Network::NativeQualification {
    namespace {
        struct ChildInput final {
            std::string directory;
            std::string nativeDirectory;
            bool rejectNativeRoot{};
            unsigned side{};
        } input;

        void ReportError(const Error &error) {
            std::cout << "NATIVE_ERROR " << error.domain.Value() << ':' << error.code.Value() << '\n' << std::flush;
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
                ++calls;
                if (reply.empty())
                    return Result<void>::Success();
                return observation->transport->Send(context.connection, {}, reply, DeliveryPolicy::ReliableOrdered);
            }

            std::shared_ptr<TransportObservation> observation;  // Revoked by the exact adapter before shutdown/destruction.
            std::byte expected{};
            std::vector<std::byte> reply;
            std::size_t calls{};
        };

        /** @brief Report only fixed source-line identity, never assertion values or private provider material. */
        class FailureLocationListener final : public Catch::EventListenerBase {
        public:
            using EventListenerBase::EventListenerBase;

            void assertionEnded(const Catch::AssertionStats &stats) override {
                if (!stats.assertionResult.isOk())
                    std::cout << "NATIVE_ASSERT " << stats.assertionResult.getSourceInfo().line << '\n' << std::flush;
            }
        };

        struct ChildOwners final {
            AdmissionTests::ProductPair fixtures;  // Reuse inert inventory and actual headless worlds only.
            std::shared_ptr<SteadyClock> clock = std::make_shared<SteadyClock>();
            std::shared_ptr<NativeHandler> handler = std::make_shared<NativeHandler>();
            std::shared_ptr<TransportObservation> observation = std::make_shared<TransportObservation>();
            std::unique_ptr<INetworkTransport> native;
            std::vector<std::byte> command;
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
            auto options =
                LoadNativeSelection(input.nativeDirectory.empty() ? input.directory : input.nativeDirectory, input.side, policy.trust);
            REQUIRE(options.HasValue());
            auto created = CreateSecureGnsTransport(std::move(options).Value());
            REQUIRE(created.HasValue());
            auto selected = std::move(created).Value();
            policy.local.transport = selected.transport->Capabilities();
            providers.certificates = selected.native.certificates;
            providers.peers = selected.native.peers;
            providers.privateKeys = selected.native.privateKeys;
            providers.channel = selected.native.channel;
            auto observed = std::make_unique<ObservedTransport>(std::move(selected.transport), owners.observation);
            owners.handler->observation = owners.observation;
            owners.handler->expected = input.side == 0 ? std::byte{0x41} : std::byte{0x42};
            if (input.side == 0)
                owners.handler->reply = Frame(owners.fixtures, std::byte{0x42});
            owners.command = Frame(owners.fixtures, std::byte{0x41});
            const std::array bindings{
                Application::Internal::NetworkProductInboundHandler{{owners.fixtures.protocols[0].id, owners.fixtures.messages[4].id,
                                                                     MessageTrafficClass::Command, Runtime::RuntimePhase::NetworkPoll, 64},
                                                                    owners.handler}};
            auto captured =
                Application::Internal::NetworkProductAdmission::Capture(policy, std::move(providers), owners.fixtures.codecs, bindings);
            REQUIRE(captured.HasValue());
            // Retain the unique adapter in the same one-shot factory ownership slot until Startup constructs its real service.
            owners.native = std::move(observed);
            return std::move(captured).Value();
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
            auto facts = ProductFacts(*owners.fixtures.project, role, false);
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
            const std::array world{input.side == 0 ? World(NetworkModeWorldKind::AuthorityServer, 1701, 1)
                                                   : World(NetworkModeWorldKind::Client, 1702)};
            auto created = Application::Internal::NetworkProductHost::Create(*owners.clock,
                                                                             {*owners.fixtures.project,
                                                                              facts.product,
                                                                              facts.inventory,
                                                                              facts.host,
                                                                              facts.requirements,
                                                                              facts.selection,
                                                                              100 + input.side,
                                                                              world,
                                                                              {.localPlayer = input.side == 1},
                                                                              sessions},
                                                                             std::move(factories));
            REQUIRE(created.HasValue());
            owners.host = std::move(created).Value();
            const auto started = owners.host->Startup();
            if (started.HasError())
                ReportError(started.ErrorValue());
            REQUIRE(started.HasValue());
            std::cout << "NATIVE_PHASE started\n" << std::flush;
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
        }

        void ShutdownAndVerifyReuse(ChildOwners &owners) {
            owners.handler->observation.reset();
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
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
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
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            bool sent{};
            while (std::chrono::steady_clock::now() < deadline) {
                auto frame = owners.host->RunFrame();
                if (frame.HasError())
                    ReportError(frame.ErrorValue());
                REQUIRE(frame.HasValue());
                if (input.side == 1 && !sent) {
                    const auto role = owners.host->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702});
                    if (role.HasValue() && role.Value().session.has_value()) {
                        REQUIRE(owners.observation->outbound.IsValid());
                        REQUIRE(owners.observation->transport
                                    ->Send(owners.observation->outbound, {}, owners.command, DeliveryPolicy::ReliableOrdered)
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
               Catch::Clara::Opt(Horo::Network::NativeQualification::input.rejectNativeRoot)["--reject-native-root"];
    session.cli(cli);
    return session.run(argc, argv);
}
