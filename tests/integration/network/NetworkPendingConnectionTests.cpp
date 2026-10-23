#include "NetworkModeSessionIngressTestSupport.h"

namespace Horo::Network {
    namespace {
        const Application::Internal::ClientConnectionAttemptTerminal *RequireAttemptTerminal(
            const Application::Internal::NetworkProductHost &host, ConnectionHandle connection, const ErrorCodeDescriptor &failure) {
            const auto *terminal = host.ClientConnectionTerminal();
            REQUIRE(terminal);
            REQUIRE(terminal->connection == connection);
            REQUIRE(terminal->failure);
            REQUIRE(terminal->failure->domain.Value() == failure.domain.Value());
            REQUIRE(terminal->failure->code.Value() == failure.code.Value());
            REQUIRE_FALSE(host.ClientSessionTerminal());
            return terminal;
        }

        void RequireRetiredAttempt(SessionProduct &product, const Application::Internal::ClientConnectionAttemptTerminal *terminal,
                                   const ErrorCodeDescriptor *failure) {
            product.owners->sessions->OnTransportEvent({.kind = NetworkTransportEventKind::Failed,
                                                        .connection = Connection(),
                                                        .failure = &NetworkErrors::TransportNativeUnavailable});
            product.host->Shutdown();
            REQUIRE(product.host->ClientConnectionTerminal() == terminal);
            REQUIRE(terminal->failure == failure);
            REQUIRE(product.owners->sessions->BeginConnectionAttempt(Connection().NextGeneration().Value()).HasError());
            REQUIRE(product.host->Startup().HasError());
        }
    }  // namespace

    TEST_CASE("Pending product connection terminals preserve exact cancel timeout and failure before any peer or travel callback",
              "[integration][network][mode][pending]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        SessionProduct product;
        product.ingress->transport.acceptedConnect = Connection();
        REQUIRE(product.host->Startup().HasValue());
        const ErrorCodeDescriptor *failure = &NetworkErrors::TransportConnectionFailed;
        SECTION("Connection rejection") {}
        SECTION("Connection cancellation") {
            failure = &NetworkErrors::TransportOperationCancelled;
        }
        SECTION("Connection timeout") {
            failure = &NetworkErrors::SessionTimedOut;
        }
        REQUIRE_FALSE(product.host->ClientConnectionTerminal());
        REQUIRE_FALSE(product.host->ClientSessionTerminal());
        REQUIRE(product.owners->sessions->BeginConnectionAttempt(Connection()).HasError());
        const auto stale = ConnectionHandle::Create(Connection().Slot(), Connection().Generation() - 1).Value();
        product.ingress->transport.Queue(
            {.kind = NetworkTransportEventKind::Failed, .connection = stale, .failure = &NetworkErrors::TransportNativeUnavailable});
        REQUIRE(product.host->RunFrame().HasValue());
        REQUIRE_FALSE(product.host->ClientConnectionTerminal());
        std::size_t worldCallbacks{};
        product.owners->beforeWorld = [&](const Runtime::RuntimePhase phase) {
            // Early housekeeping precedes transport polling; the network fence applies before postpoll world work.
            if (phase >= Runtime::RuntimePhase::NetworkPoll)
                ++worldCallbacks;
        };
        const std::array nextWorld{World(NetworkModeWorldKind::Client, 3)};
        REQUIRE(product.host->RequestTravel(nextWorld).HasValue());
        product.ingress->transport.Queue({.kind = NetworkTransportEventKind::Failed, .connection = Connection(), .failure = failure});
        const auto frame = product.host->RunFrame();
        REQUIRE(frame.HasError());
        REQUIRE(frame.ErrorValue().domain.Value() == failure->domain.Value());
        REQUIRE(frame.ErrorValue().code.Value() == failure->code.Value());
        REQUIRE(worldCallbacks == 0);
        const auto *terminal = RequireAttemptTerminal(*product.host, Connection(), *failure);
        REQUIRE(terminal->failure == failure);
        REQUIRE(product.ingress->transport.stopped);
        REQUIRE(product.host->Role(NetworkModeWorldKind::Client, product.worlds[0].scene).HasError());
        auto travelFailure = product.host->TakeTravelFailure();
        REQUIRE(travelFailure);
        REQUIRE(travelFailure->domain.Value() == failure->domain.Value());
        REQUIRE(travelFailure->code.Value() == failure->code.Value());
        RequireRetiredAttempt(product, terminal, failure);
    }

    TEST_CASE("Pending product shutdown and partial preparation never invent a peer or overwrite an original attempt terminal",
              "[integration][network][mode][pending]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        {
            SessionProduct product;
            product.ingress->transport.acceptedConnect = Connection();
            SECTION("Shutdown retires an accepted unready attempt once") {
                REQUIRE(product.host->Startup().HasValue());
                product.host->Shutdown();
                const auto *terminal = RequireAttemptTerminal(*product.host, Connection(), NetworkErrors::SessionShuttingDown);
                product.host->Shutdown();
                REQUIRE(product.host->ClientConnectionTerminal() == terminal);
                REQUIRE(product.ingress->transport.stopped);
            }
            SECTION("Failure before Activate never accepts a connection attempt") {
                product.ingress->failAfterAttach = true;
                REQUIRE(product.host->Startup().HasError());
                REQUIRE_FALSE(product.host->ClientConnectionTerminal());
                REQUIRE_FALSE(product.host->ClientSessionTerminal());
                REQUIRE(product.ingress->transport.stopped);
            }
        }
        SessionProduct fresh;
        REQUIRE(fresh.host->Startup().HasValue());
        REQUIRE(fresh.host->RunFrame().HasValue());
        REQUIRE_FALSE(fresh.host->ClientConnectionTerminal());
        REQUIRE_FALSE(fresh.host->ClientSessionTerminal());
    }

    TEST_CASE("Listen product pending outbound attempt is not consumed by unrelated inbound terminal or server admission",
              "[integration][network][mode][pending]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        DeterministicClock clock;
        auto owners = std::make_shared<ProductOwners>();
        auto ingress = InstallIngress(owners);
        ingress->transport.acceptedConnect = Connection();
        const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 1, 1), World(NetworkModeWorldKind::Client, 2)};
        auto created = Product(clock, NetworkProjectRole::ListenServer, worlds, {.localPlayer = true}, owners);
        REQUIRE(created.HasValue());
        auto host = std::move(created).Value();
        REQUIRE(host->Startup().HasValue());
        REQUIRE(owners->sessions->OnSessionPrepared(AdmissionEndpoint::Server, nullptr, {}, 22).HasValue());
        ingress->transport.Queue({.kind = NetworkTransportEventKind::Failed,
                                  .connection = Connection().NextGeneration().Value(),
                                  .failure = &NetworkErrors::TransportNativeUnavailable});
        REQUIRE(host->RunFrame().HasValue());
        REQUIRE_FALSE(host->ClientConnectionTerminal());
        REQUIRE(host->Role(NetworkModeWorldKind::AuthorityServer, worlds[0].scene).HasValue());
        ingress->transport.Queue(
            {.kind = NetworkTransportEventKind::Failed, .connection = Connection(), .failure = &NetworkErrors::TransportConnectionFailed});
        const auto failed = host->RunFrame();
        REQUIRE(failed.HasError());
        REQUIRE(failed.ErrorValue().code.Value() == NetworkErrors::TransportConnectionFailed.code.Value());
        REQUIRE(host->ClientConnectionTerminal());
        REQUIRE(host->ClientConnectionTerminal()->connection == Connection());
        REQUIRE_FALSE(host->ClientSessionTerminal());
        REQUIRE(host->Role(NetworkModeWorldKind::AuthorityServer, worlds[0].scene).HasError());
        REQUIRE(host->Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());
    }
}  // namespace Horo::Network
