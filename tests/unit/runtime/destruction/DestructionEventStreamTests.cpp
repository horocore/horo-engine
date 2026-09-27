#include "Horo/Destruction/DestructionEventStream.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>

namespace Horo::Destruction {
    namespace {
        template <typename Identity> Identity Id(const std::uint64_t value) {
            return Identity::Create(value).Value();
        }

        DestructionHandle Handle(const std::uint64_t world = 7, const std::uint64_t generation = 1) {
            return {Id<DestructionWorldId>(world), Id<DestructibleId>(11), Id<DestructionGeneration>(generation)};
        }

        DestructionFact Fact(const std::uint64_t revision, const std::uint32_t ordinal = 0, const std::uint64_t ticket = 1,
                             const std::uint64_t tick = 1) {
            return {.occurrence = {.source = Handle(),
                                   .stateRevision = Id<DestructionStateRevision>(revision),
                                   .kind = DestructionFactKind::Damaged,
                                   .revisionOrdinal = ordinal},
                    .transitionTicket = ticket,
                    .committedTick = tick,
                    .payload = {.strength = 2.0F}};
        }

        std::unique_ptr<DestructionEventStream> Stream(const std::uint32_t capacity = 3) {
            auto [status, stream] = DestructionEventStream::Create(Handle().world, capacity, capacity);
            REQUIRE(status == DestructionEventStatus::Ok);
            return std::move(stream);
        }

        void PublishOne(DestructionEventStream &stream, const DestructionFact &fact, const DestructionEventCursor requiredCursor) {
            auto [status, reservation] = stream.Reserve(Handle(), fact.transitionTicket, std::span(&fact, 1), requiredCursor);
            REQUIRE(status == DestructionEventStatus::Ok);
            REQUIRE(stream.Publish(std::move(reservation), Handle()) == DestructionEventStatus::Ok);
        }
    }  // namespace

    TEST_CASE("Post-commit journal publishes one ordered immutable fact after reservation", "[unit][destruction][event]") {
        auto stream = Stream();
        const DestructionFact fact = Fact(1);
        auto [status, reservation] = stream->Reserve(Handle(), 1, std::span(&fact, 1), stream->Oldest());
        REQUIRE(status == DestructionEventStatus::Ok);
        CHECK(stream->Read(stream->Oldest()).status == DestructionEventStatus::Empty);
        CHECK(stream->Publish(std::move(reservation), Handle()) == DestructionEventStatus::Ok);
        const auto read = stream->Read(stream->Oldest());
        REQUIRE(read.status == DestructionEventStatus::Ok);
        CHECK(read.fact.occurrence == fact.occurrence);
        CHECK(read.next == stream->Tail());
        CHECK(stream->Read(read.next).status == DestructionEventStatus::Empty);
    }

    TEST_CASE("Cancelled and stale candidates publish no facts", "[unit][destruction][event]") {
        auto stream = Stream();
        const DestructionFact fact = Fact(1);
        auto [status, stale] = stream->Reserve(Handle(), 1, std::span(&fact, 1), stream->Oldest());
        REQUIRE(status == DestructionEventStatus::Ok);
        PublishOne(*stream, fact, stream->Oldest());
        CHECK(stream->Publish(std::move(stale), Handle()) == DestructionEventStatus::ReservationStale);
        const DestructionFact later = Fact(2, 0, 2, 2);
        auto [nextStatus, next] = stream->Reserve(Handle(), 2, std::span(&later, 1), stream->Oldest());
        REQUIRE(nextStatus == DestructionEventStatus::Ok);
        CHECK(stream->Publish(std::move(next), Handle(7, 2)) == DestructionEventStatus::StaleGeneration);
        CHECK(stream->Tail().sequence == 1);
    }

    TEST_CASE("Required lag blocks admission while optional lag yields a bounded snapshot gap", "[unit][destruction][event]") {
        auto stream = Stream(2);
        PublishOne(*stream, Fact(1), stream->Oldest());
        PublishOne(*stream, Fact(2, 0, 2, 2), stream->Oldest());
        const DestructionFact later = Fact(3, 0, 3, 3);
        CHECK(stream->Reserve(Handle(), 3, std::span(&later, 1), stream->Oldest()).first ==
              DestructionEventStatus::RequiredConsumerStalled);
        PublishOne(*stream, Fact(3, 0, 3, 3), stream->Tail());
        const auto gap = stream->Read({Handle().world, 0});
        REQUIRE(gap.status == DestructionEventStatus::Gap);
        CHECK(gap.next.sequence == 1);
        CHECK(stream->Read(gap.next).fact.occurrence.stateRevision == Id<DestructionStateRevision>(2));
    }

    TEST_CASE("Malformed or stale source batches never partially append", "[unit][destruction][event]") {
        auto stream = Stream();
        std::array<DestructionFact, 2> facts{Fact(1), Fact(1, 1)};
        facts[1].payload.strength = std::numeric_limits<float>::quiet_NaN();
        CHECK(stream->Reserve(Handle(), 1, facts, stream->Oldest()).first == DestructionEventStatus::Invalid);
        CHECK(stream->Tail().sequence == 0);
        facts[1].payload.strength = 1.0F;
        auto [status, reservation] = stream->Reserve(Handle(), 1, facts, stream->Oldest());
        REQUIRE(status == DestructionEventStatus::Ok);
        CHECK(stream->Publish(std::move(reservation), Handle()) == DestructionEventStatus::Ok);
        CHECK(stream->Reserve(Handle(8), 2, std::span(facts).first(1), stream->Tail()).first == DestructionEventStatus::Invalid);
        const DestructionFact duplicateRevision = Fact(1, 0, 2, 2);
        CHECK(stream->Reserve(Handle(), 2, std::span(&duplicateRevision, 1), stream->Tail()).first ==
              DestructionEventStatus::StaleRevision);
    }

    TEST_CASE("Owner-thread and shutdown fences preserve the readable final prefix", "[unit][destruction][event]") {
        auto stream = Stream();
        PublishOne(*stream, Fact(1), stream->Oldest());
        DestructionEventStatus crossThread{};
        std::thread worker([&] {
            crossThread = stream->Read(stream->Oldest()).status;
        });
        worker.join();
        CHECK(crossThread == DestructionEventStatus::WrongThread);
        stream->BeginShutdown();
        const DestructionFact later = Fact(2, 0, 2, 2);
        CHECK(stream->Reserve(Handle(), 2, std::span(&later, 1), stream->Tail()).first == DestructionEventStatus::ShutdownInProgress);
        CHECK(stream->Read(stream->Oldest()).status == DestructionEventStatus::Ok);
    }
}  // namespace Horo::Destruction
