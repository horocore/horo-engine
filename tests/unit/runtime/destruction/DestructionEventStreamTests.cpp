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
                             const std::uint64_t tick = 1, const DestructionHandle source = Handle()) {
            return {.occurrence = {.source = source,
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
            const auto sourceRevision = Id<DestructionStateRevision>(fact.occurrence.stateRevision.Value() - 1);
            auto [status, reservation] =
                stream.Reserve(fact.occurrence.source, sourceRevision, fact.transitionTicket, std::span(&fact, 1), requiredCursor);
            REQUIRE(status == DestructionEventStatus::Ok);
            REQUIRE(stream.Publish(std::move(reservation), fact.occurrence.source, fact.occurrence.stateRevision) ==
                    DestructionEventStatus::Ok);
        }
    }  // namespace

    TEST_CASE("Post-commit journal publishes one ordered immutable fact after reservation", "[unit][destruction][event]") {
        auto stream = Stream();
        const DestructionFact fact = Fact(2);
        auto [status, reservation] = stream->Reserve(Handle(), Id<DestructionStateRevision>(1), 1, std::span(&fact, 1), stream->Oldest());
        REQUIRE(status == DestructionEventStatus::Ok);
        CHECK(stream->Read(stream->Oldest()).status == DestructionEventStatus::Empty);
        CHECK(stream->Publish(std::move(reservation), Handle(), fact.occurrence.stateRevision) == DestructionEventStatus::Ok);
        const auto read = stream->Read(stream->Oldest());
        REQUIRE(read.status == DestructionEventStatus::Ok);
        CHECK(read.fact.occurrence == fact.occurrence);
        CHECK(read.next == stream->Tail());
        CHECK(stream->Read(read.next).status == DestructionEventStatus::Empty);
    }

    TEST_CASE("Cancelled and stale candidates publish no facts", "[unit][destruction][event]") {
        auto stream = Stream();
        const DestructionFact fact = Fact(2);
        auto [status, stale] = stream->Reserve(Handle(), Id<DestructionStateRevision>(1), 1, std::span(&fact, 1), stream->Oldest());
        REQUIRE(status == DestructionEventStatus::Ok);
        PublishOne(*stream, fact, stream->Oldest());
        CHECK(stream->Publish(std::move(stale), Handle(), fact.occurrence.stateRevision) == DestructionEventStatus::ReservationStale);
        const DestructionFact later = Fact(3, 0, 2, 2);
        auto [nextStatus, next] = stream->Reserve(Handle(), Id<DestructionStateRevision>(2), 2, std::span(&later, 1), stream->Oldest());
        REQUIRE(nextStatus == DestructionEventStatus::Ok);
        CHECK(stream->Publish(std::move(next), Handle(7, 2), later.occurrence.stateRevision) == DestructionEventStatus::StaleGeneration);
        CHECK(stream->Tail().sequence == 1);
    }

    TEST_CASE("Required lag blocks admission while optional lag yields a bounded snapshot gap", "[unit][destruction][event]") {
        auto stream = Stream(2);
        PublishOne(*stream, Fact(2), stream->Oldest());
        PublishOne(*stream, Fact(3, 0, 2, 2), stream->Oldest());
        const DestructionFact later = Fact(4, 0, 3, 3);
        CHECK(stream->Reserve(Handle(), Id<DestructionStateRevision>(3), 3, std::span(&later, 1), stream->Oldest()).first ==
              DestructionEventStatus::RequiredConsumerStalled);
        PublishOne(*stream, later, stream->Tail());
        const auto gap = stream->Read({Handle().world, 0});
        REQUIRE(gap.status == DestructionEventStatus::Gap);
        CHECK(gap.next.sequence == 1);
        CHECK(stream->Read(gap.next).fact.occurrence.stateRevision == Id<DestructionStateRevision>(3));
    }

    TEST_CASE("Malformed or stale source batches never partially append", "[unit][destruction][event]") {
        auto stream = Stream();
        std::array<DestructionFact, 2> facts{Fact(2), Fact(2, 1)};
        facts[1].payload.strength = std::numeric_limits<float>::quiet_NaN();
        CHECK(stream->Reserve(Handle(), Id<DestructionStateRevision>(1), 1, facts, stream->Oldest()).first ==
              DestructionEventStatus::Invalid);
        CHECK(stream->Tail().sequence == 0);
        facts[1].payload.strength = 1.0F;
        auto [status, reservation] = stream->Reserve(Handle(), Id<DestructionStateRevision>(1), 1, facts, stream->Oldest());
        REQUIRE(status == DestructionEventStatus::Ok);
        CHECK(stream->Publish(std::move(reservation), Handle(), facts[0].occurrence.stateRevision) == DestructionEventStatus::Ok);
        CHECK(stream->Reserve(Handle(8), Id<DestructionStateRevision>(1), 2, std::span(facts).first(1), stream->Tail()).first ==
              DestructionEventStatus::Invalid);
        const DestructionFact duplicateRevision = Fact(2, 0, 2, 2);
        CHECK(stream->Reserve(Handle(), Id<DestructionStateRevision>(2), 2, std::span(&duplicateRevision, 1), stream->Tail()).first ==
              DestructionEventStatus::StaleRevision);
    }

    TEST_CASE("Interleaved and evicted facts cannot roll back a canonical source revision", "[unit][destruction][event]") {
        auto stream = Stream(2);
        const DestructionHandle sourceA = Handle();
        const DestructionHandle sourceB{sourceA.world, Id<DestructibleId>(12), sourceA.generation};
        PublishOne(*stream, Fact(2, 0, 1, 1, sourceA), stream->Tail());
        PublishOne(*stream, Fact(2, 0, 2, 2, sourceB), stream->Tail());

        const DestructionFact staleA = Fact(1, 0, 3, 3, sourceA);
        CHECK(stream->Reserve(sourceA, Id<DestructionStateRevision>(2), 3, std::span(&staleA, 1), stream->Tail()).first ==
              DestructionEventStatus::StaleRevision);
        const DestructionFact skippedA = Fact(4, 0, 3, 3, sourceA);
        CHECK(stream->Reserve(sourceA, Id<DestructionStateRevision>(2), 3, std::span(&skippedA, 1), stream->Tail()).first ==
              DestructionEventStatus::StaleRevision);
        CHECK(stream
                  ->Reserve(sourceA, Id<DestructionStateRevision>(std::numeric_limits<std::uint64_t>::max()), 3, std::span(&skippedA, 1),
                            stream->Tail())
                  .first == DestructionEventStatus::StaleRevision);

        PublishOne(*stream, Fact(3, 0, 3, 3, sourceB), stream->Tail());
        REQUIRE(stream->Oldest().sequence == 1);
        CHECK(stream->Reserve(sourceA, Id<DestructionStateRevision>(2), 3, std::span(&staleA, 1), stream->Tail()).first ==
              DestructionEventStatus::StaleRevision);
        CHECK(stream->Tail().sequence == 3);

        const DestructionFact replacement = Fact(2, 0, 4, 4, Handle(7, 2));
        CHECK(stream->Reserve(sourceA, Id<DestructionStateRevision>(1), 4, std::span(&replacement, 1), stream->Tail()).first ==
              DestructionEventStatus::Invalid);
        PublishOne(*stream, replacement, stream->Tail());
        CHECK(stream->Tail().sequence == 4);
    }

    TEST_CASE("Publication requires the exact committed source revision", "[unit][destruction][event]") {
        auto stream = Stream();
        const DestructionFact fact = Fact(2);
        auto [status, reservation] = stream->Reserve(Handle(), Id<DestructionStateRevision>(1), 1, std::span(&fact, 1), stream->Tail());
        REQUIRE(status == DestructionEventStatus::Ok);
        CHECK(stream->Publish(std::move(reservation), Handle(), Id<DestructionStateRevision>(3)) == DestructionEventStatus::StaleRevision);
        CHECK(stream->Tail().sequence == 0);
        CHECK(stream->Publish(std::move(reservation), Handle(), fact.occurrence.stateRevision) == DestructionEventStatus::Ok);
    }

    TEST_CASE("Owner-thread and shutdown fences preserve the readable final prefix", "[unit][destruction][event]") {
        auto stream = Stream();
        PublishOne(*stream, Fact(2), stream->Oldest());
        DestructionEventStatus crossThread{};
        std::thread worker([&] {
            crossThread = stream->Read(stream->Oldest()).status;
        });
        worker.join();
        CHECK(crossThread == DestructionEventStatus::WrongThread);
        stream->BeginShutdown();
        const DestructionFact later = Fact(3, 0, 2, 2);
        CHECK(stream->Reserve(Handle(), Id<DestructionStateRevision>(2), 2, std::span(&later, 1), stream->Tail()).first ==
              DestructionEventStatus::ShutdownInProgress);
        CHECK(stream->Read(stream->Oldest()).status == DestructionEventStatus::Ok);
    }
}  // namespace Horo::Destruction
