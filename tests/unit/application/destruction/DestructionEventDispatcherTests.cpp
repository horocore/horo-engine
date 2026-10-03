#include "Horo/Destruction/DestructionEventDispatcher.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::Destruction {
    namespace {
        template <typename Identity> Identity Id(const std::uint64_t value) {
            return Identity::Create(value).Value();
        }

        DestructionHandle Handle(const std::uint64_t generation = 1) {
            return {Id<DestructionWorldId>(7), Id<DestructibleId>(11), Id<DestructionGeneration>(generation)};
        }

        DestructionFact Fact(const std::uint64_t revision = 2) {
            return {.occurrence = {.source = Handle(),
                                   .stateRevision = Id<DestructionStateRevision>(revision),
                                   .kind = DestructionFactKind::Damaged},
                    .transitionTicket = revision,
                    .committedTick = revision,
                    .payload = {.strength = 2.0F}};
        }

        std::unique_ptr<DestructionEventStream> Stream() {
            auto [status, stream] = DestructionEventStream::Create(Handle().world, 3, 2);
            REQUIRE(status == DestructionEventStatus::Ok);
            return std::move(stream);
        }

        void Publish(DestructionEventStream &stream, const DestructionFact &fact) {
            const auto sourceRevision = Id<DestructionStateRevision>(fact.occurrence.stateRevision.Value() - 1);
            auto [status, reservation] =
                stream.Reserve(Handle(), sourceRevision, fact.transitionTicket, std::span(&fact, 1), stream.Tail());
            REQUIRE(status == DestructionEventStatus::Ok);
            REQUIRE(stream.Publish(std::move(reservation), Handle(), fact.occurrence.stateRevision) == DestructionEventStatus::Ok);
        }

        constexpr DestructionEventBinding Gameplay{.factKind = DestructionFactKind::Damaged,
                                                   .destination = DestructionDestinationKind::Gameplay,
                                                   .destinationId = 1,
                                                   .required = true,
                                                   .headlessEligible = true};
        constexpr DestructionEventBinding Audio{.factKind = DestructionFactKind::Damaged,
                                                .destination = DestructionDestinationKind::Audio,
                                                .destinationId = 2,
                                                .layerOrdinal = 1};

        DestructionEventDispatcher Dispatcher(const bool headless = false) {
            const std::array bindings{Gameplay, Audio};
            auto [status, dispatcher] = DestructionEventDispatcher::Create(Handle().world, 3, bindings, headless);
            REQUIRE(status == DestructionEventStatus::Ok);
            return dispatcher;
        }

        struct Adapter final : IDestructionDestinationAdapter {
            bool supports{true};
            DestructionEventStatus reserveOutcome{DestructionEventStatus::Ok};
            DestructionEventStatus submitOutcome{DestructionEventStatus::Ok};
            std::uint32_t reserved{};
            std::uint32_t cancelled{};
            std::uint32_t submitted{};
            DestructionDestinationRequestId last{};
            std::uint64_t lastTicket{};

            bool Supports(const std::uint16_t schema) const noexcept override {
                return supports && schema == 1;
            }

            DestructionEventStatus ReserveRequired(const std::uint64_t transitionTicket, const std::uint32_t count) noexcept override {
                lastTicket = transitionTicket;
                reserved += count;
                return reserveOutcome;
            }

            void CancelRequired(const std::uint64_t transitionTicket) noexcept override {
                lastTicket = transitionTicket;
                ++cancelled;
            }

            DestructionEventStatus Submit(const DestructionDestinationRequest &request) noexcept override {
                ++submitted;
                last = request.id;
                return submitOutcome;
            }
        };

        std::array<DestructionAdapterSlot, 2> Slots(Adapter &gameplay, Adapter &audio) {
            return {{{DestructionDestinationKind::Gameplay, 1, &gameplay}, {DestructionDestinationKind::Audio, 2, &audio}}};
        }
    }  // namespace

    TEST_CASE("Cooked binding preflight reserves required gameplay and rejects missing capacity", "[unit][destruction][adapter]") {
        auto dispatcher = Dispatcher();
        Adapter gameplay, audio;
        const auto slots = Slots(gameplay, audio);
        const std::array facts{Fact()};
        CHECK(dispatcher.Preflight(facts, slots) == DestructionEventStatus::Ok);
        CHECK(gameplay.reserved == 1);
        CHECK(audio.reserved == 0);
        gameplay.reserveOutcome = DestructionEventStatus::ConsumerCapacityExceeded;
        CHECK(dispatcher.Preflight(facts, slots) == DestructionEventStatus::ConsumerCapacityExceeded);
        CHECK(dispatcher.Preflight(facts, std::span(slots).last(1)) == DestructionEventStatus::ConsumerUnavailable);
        gameplay.supports = false;
        CHECK(dispatcher.Preflight(facts, slots) == DestructionEventStatus::ConsumerUnavailable);
    }

    TEST_CASE("Safe-point fan-out uses copied values and stable per-layer identities", "[unit][destruction][adapter]") {
        auto stream = Stream();
        const auto fact = Fact();
        Publish(*stream, fact);
        auto dispatcher = Dispatcher();
        Adapter gameplay, audio;
        const auto slots = Slots(gameplay, audio);
        const auto result = dispatcher.PumpOne(*stream, Handle(), slots);
        REQUIRE(result.status == DestructionEventStatus::Ok);
        CHECK(result.submitted == 2);
        CHECK(result.next == stream->Tail());
        CHECK(gameplay.last.occurrence == fact.occurrence);
        CHECK(gameplay.last.bindingGeneration == 3);
        CHECK(audio.last.layerOrdinal == 1);
        CHECK(dispatcher.PumpOne(*stream, Handle(), slots).status == DestructionEventStatus::Empty);
    }

    TEST_CASE("Required failure retains the exact pending layer without repeating admitted layers", "[unit][destruction][adapter]") {
        auto stream = Stream();
        Publish(*stream, Fact());
        auto dispatcher = Dispatcher();
        Adapter gameplay, audio;
        gameplay.submitOutcome = DestructionEventStatus::ConsumerCapacityExceeded;
        const auto slots = Slots(gameplay, audio);
        CHECK(dispatcher.PumpOne(*stream, Handle(), slots).status == DestructionEventStatus::ConsumerCapacityExceeded);
        CHECK(dispatcher.Cursor().sequence == 0);
        CHECK(dispatcher.ReplaceBindings(4, std::array{Gameplay, Audio}) == DestructionEventStatus::ReservationStale);
        gameplay.submitOutcome = DestructionEventStatus::Ok;
        CHECK(dispatcher.PumpOne(*stream, Handle(), slots).status == DestructionEventStatus::Ok);
        CHECK(gameplay.submitted == 2);
        CHECK(audio.submitted == 1);
    }

    TEST_CASE("Headless omits cosmetics while stale source generations cannot route", "[unit][destruction][adapter]") {
        auto stream = Stream();
        Publish(*stream, Fact());
        auto dispatcher = Dispatcher(true);
        Adapter gameplay, audio;
        const auto slots = Slots(gameplay, audio);
        CHECK(dispatcher.PumpOne(*stream, Handle(2), slots).status == DestructionEventStatus::StaleGeneration);
        const auto result = dispatcher.PumpOne(*stream, Handle(), slots);
        REQUIRE(result.status == DestructionEventStatus::Ok);
        CHECK(result.submitted == 1);
        CHECK(result.suppressed == 1);
        CHECK(audio.submitted == 0);
    }

    TEST_CASE("Optional admission failure does not roll back required semantic delivery", "[unit][destruction][adapter]") {
        auto stream = Stream();
        Publish(*stream, Fact());
        auto dispatcher = Dispatcher();
        Adapter gameplay, audio;
        audio.submitOutcome = DestructionEventStatus::ConsumerCapacityExceeded;
        const auto result = dispatcher.PumpOne(*stream, Handle(), Slots(gameplay, audio));
        REQUIRE(result.status == DestructionEventStatus::Ok);
        CHECK(result.submitted == 1);
        CHECK(result.suppressed == 1);
        CHECK(dispatcher.Cursor() == stream->Tail());
    }

    TEST_CASE("Cook rejects duplicate mapping and unavailable required headless output", "[unit][destruction][adapter]") {
        const std::array duplicates{Gameplay, Gameplay};
        CHECK(DestructionEventDispatcher::Create(Handle().world, 1, duplicates, false).first == DestructionEventStatus::Invalid);
        auto ineligible = Gameplay;
        ineligible.headlessEligible = false;
        CHECK(DestructionEventDispatcher::Create(Handle().world, 1, std::span(&ineligible, 1), true).first ==
              DestructionEventStatus::Invalid);
    }

    TEST_CASE("Preflight checks every required schema even when layers share one destination", "[unit][destruction][adapter]") {
        auto alternate = Gameplay;
        alternate.factKind = DestructionFactKind::ChunksActivated;
        alternate.requestSchema = 2;
        const std::array bindings{Gameplay, alternate};
        auto [status, dispatcher] = DestructionEventDispatcher::Create(Handle().world, 3, bindings, false);
        REQUIRE(status == DestructionEventStatus::Ok);
        Adapter gameplay, audio;
        const std::array facts{Fact()};
        CHECK(dispatcher.Preflight(facts, Slots(gameplay, audio)) == DestructionEventStatus::ConsumerUnavailable);
        CHECK(gameplay.reserved == 0);
    }

    TEST_CASE("Failed later required reservation cancels earlier capacity and explicit rollback is idempotent",
              "[unit][destruction][adapter]") {
        auto accessibility = Gameplay;
        accessibility.destination = DestructionDestinationKind::Accessibility;
        accessibility.destinationId = 3;
        const std::array bindings{Gameplay, accessibility};
        auto [status, dispatcher] = DestructionEventDispatcher::Create(Handle().world, 3, bindings, true);
        REQUIRE(status == DestructionEventStatus::Ok);
        Adapter gameplay, caption;
        caption.reserveOutcome = DestructionEventStatus::ConsumerCapacityExceeded;
        const std::array slots{DestructionAdapterSlot{DestructionDestinationKind::Gameplay, 1, &gameplay},
                               DestructionAdapterSlot{DestructionDestinationKind::Accessibility, 3, &caption}};
        const std::array facts{Fact()};
        CHECK(dispatcher.Preflight(facts, slots) == DestructionEventStatus::ConsumerCapacityExceeded);
        CHECK(gameplay.reserved == 1);
        CHECK(gameplay.cancelled == 1);
        CHECK(dispatcher.CancelRequired(1, slots) == DestructionEventStatus::Ok);
        CHECK(gameplay.cancelled == 2);
    }

    TEST_CASE("Dispatcher owner-thread fence rejects cross-thread delivery", "[unit][destruction][adapter]") {
        auto stream = Stream();
        auto dispatcher = Dispatcher();
        Adapter gameplay, audio;
        DestructionEventStatus outcome{};
        std::thread worker([&] {
            outcome = dispatcher.PumpOne(*stream, Handle(), Slots(gameplay, audio)).status;
        });
        worker.join();
        CHECK(outcome == DestructionEventStatus::WrongThread);
        CHECK(dispatcher.Cursor().sequence == 0);
    }

    TEST_CASE("Replacement, reconciliation and shutdown fence dispatcher lifetimes", "[unit][destruction][adapter]") {
        auto dispatcher = Dispatcher();
        auto stream = Stream();
        Publish(*stream, Fact());
        CHECK(dispatcher.Reconcile(*stream, stream->Tail()) == DestructionEventStatus::Ok);
        CHECK(dispatcher.Reconcile(*stream, {Handle().world, 2}) == DestructionEventStatus::Invalid);
        CHECK(dispatcher.ReplaceBindings(4, std::array{Gameplay}) == DestructionEventStatus::Ok);
        CHECK(dispatcher.ReplaceBindings(3, std::array{Gameplay}) == DestructionEventStatus::StaleRevision);
        CHECK(dispatcher.Reconcile(*stream, {Id<DestructionWorldId>(8), 3}) == DestructionEventStatus::StaleGeneration);
        dispatcher.BeginShutdown();
        CHECK(dispatcher.ReplaceBindings(5, std::array{Gameplay}) == DestructionEventStatus::ShutdownInProgress);
    }
}  // namespace Horo::Destruction
