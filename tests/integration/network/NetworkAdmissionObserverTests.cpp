#include "NetworkAdmissionProductTestSupport.h"

#include <type_traits>

namespace Horo::Network::AdmissionTests {
    namespace {
        /** @brief Read-only lease projection never mutates authentication or lifecycle state. */
        class Projection final : public INetworkAdmissionObserver {
        public:
            Result<void> OnSessionPrepared(AdmissionEndpoint, const std::shared_ptr<PeerSessionLifecycle> &value,
                                           NetworkOperationGeneration, std::uint64_t) override {
                peer = value;
                ++prepared;
                return Result<void>::Success();
            }

            Result<void> OnSessionActivated(AdmissionEndpoint, const std::shared_ptr<PeerSessionLifecycle> &value,
                                            NetworkOperationGeneration, std::uint64_t) override {
                REQUIRE(value == peer);
                ++activated;
                return Result<void>::Success();
            }

            std::shared_ptr<PeerSessionLifecycle> peer;
            std::size_t prepared{};
            std::size_t activated{};
        };

        /** @brief Same runtime controller/router/provider path with independently expirable projection leases. */
        struct ControllerPair final {
            ControllerPair() {
                for (std::size_t side = 0; side < 2; ++side) {
                    transports[side] = std::make_unique<PairedTransport>(inputs.link, side);
                    auto router = InboundMessageDispatcher::Create(*transports[side], *inputs.codecs);
                    REQUIRE(router.HasValue());
                    routers[side] = std::move(router).Value();
                    observers[side] = std::make_shared<Projection>();
                    auto controller =
                        NetworkAdmissionController::Create(*transports[side], *routers[side], inputs.codecs, inputs.Policy(side),
                                                           inputs.authorities[side]->Providers(inputs.authorities[side]), observers[side]);
                    REQUIRE(controller.HasValue());
                    controllers[side] = std::move(controller).Value();
                    routers[side]->SetAdmissionController(controllers[side]);
                    REQUIRE(transports[side]->Initialize({4, 64, 1200}).HasValue());
                }
                REQUIRE(transports[0]->Listen({}).HasValue());
                REQUIRE(transports[1]->Connect({}).HasValue());
            }

            ~ControllerPair() {
                for (auto &controller : controllers)
                    controller->Shutdown(22);
                for (auto &router : routers)
                    router->Shutdown();
            }

            void Polls(const std::size_t count) {
                for (std::size_t i = 0; i < count; ++i)
                    for (std::size_t side = 0; side < 2; ++side) {
                        REQUIRE(controllers[side]->BeginPoll(22).HasValue());
                        REQUIRE(routers[side]->RunNetworkPoll(22).HasValue());
                    }
            }

            ProductPair inputs;
            std::array<std::unique_ptr<PairedTransport>, 2> transports;
            std::array<std::unique_ptr<InboundMessageDispatcher>, 2> routers;
            std::array<std::shared_ptr<Projection>, 2> observers;
            std::array<std::shared_ptr<NetworkAdmissionController>, 2> controllers;
        };

        /** @brief Capture one actual native connectivity event without synthesizing session authority. */
        struct Connectivity final : INetworkTransportEventConsumer {
            void Consume(NetworkTransportEvent value) noexcept override {
                static_assert(std::is_nothrow_move_assignable_v<NetworkTransportEvent>);
                if (count != 0) {
                    overflow = true;
                    return;
                }
                event = std::move(value);
                count = 1;
            }

            NetworkTransportEvent event;
            std::size_t count{};
            bool overflow{};
        };
    }  // namespace

    TEST_CASE("Required client projection expiry before pending publication is local cancellation rather than peer authentication failure",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("The shared product policy fixture uses the explicit Native Physics composition.");
        ControllerPair pair;
        pair.observers[1].reset();
        Connectivity native;
        REQUIRE(pair.transports[1]->PollEvents(native).HasValue());
        REQUIRE_FALSE(native.overflow);
        REQUIRE(native.count == 1);
        REQUIRE(native.event.kind == NetworkTransportEventKind::Connected);
        REQUIRE(pair.controllers[1]->BeginPoll(22).HasValue());
        const auto outcome = pair.controllers[1]->OnTransportEvent(native.event, 22);
        REQUIRE(outcome.HasValue());
        CHECK(outcome.Value() == AdmissionEventDisposition::CandidateCancelled);
        CHECK_FALSE(pair.inputs.link->ends[1].open);
        CHECK(pair.inputs.authorities[1]->issuerCalls == 0);
        const auto closes = pair.inputs.link->ends[1].closes;
        pair.controllers[1]->Shutdown(22);
        pair.controllers[1]->Shutdown(22);
        CHECK(pair.inputs.link->ends[1].closes == closes);
    }

    TEST_CASE("Required client projection expiry before activation cancels the same authenticated candidate before gameplay publication",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("The shared product policy fixture uses the explicit Native Physics composition.");
        ControllerPair pair;
        pair.Polls(2);
        auto peer = pair.observers[1]->peer;
        REQUIRE(peer);
        REQUIRE(peer->State() == PeerSessionState::Activating);
        REQUIRE(peer->Authentication());
        pair.observers[1].reset();
        pair.Polls(4);
        REQUIRE(peer->Terminal());
        CHECK(peer->Terminal()->kind == PeerSessionTerminalKind::LocalCancellation);
        CHECK_FALSE(peer->AdmitGameplay(peer->Connection(), peer->Negotiation()->sessionGeneration, 22).HasValue());
        CHECK_FALSE(pair.inputs.link->ends[1].open);
        const auto *terminal = peer->Terminal();
        const auto closes = pair.inputs.link->ends[1].closes;
        pair.controllers[1]->Shutdown(22);
        CHECK(peer->Terminal() == terminal);
        CHECK(pair.inputs.link->ends[1].closes == closes);
    }

    TEST_CASE("Server-only optional projection expiry does not prevent independent canonical activation",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("The shared product policy fixture uses the explicit Native Physics composition.");
        ControllerPair pair;
        pair.observers[0].reset();
        pair.Polls(5);
        REQUIRE(pair.observers[1]->peer);
        CHECK(pair.observers[1]->peer->State() == PeerSessionState::Active);
        CHECK(pair.observers[1]->activated == 1);
        CHECK(pair.inputs.link->ends[0].open);
        CHECK(pair.inputs.authorities[0]->credentialCalls == 1);
        CHECK(pair.inputs.authorities[1]->credentialCalls == 1);
    }
}  // namespace Horo::Network::AdmissionTests
