#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkTickAlignment.h"
#include "NetworkTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>

namespace Horo::Network {
    using TestSupport::RequireError;

    namespace {
        [[nodiscard]] ConnectionHandle Connection(const std::uint32_t generation = 1) {
            return ConnectionHandle::Create(3, generation).Value();
        }

        [[nodiscard]] NetworkOperationGeneration Session(const std::uint64_t generation = 7) {
            return NetworkOperationGeneration::Create(generation).Value();
        }

        [[nodiscard]] NetworkTickAlignment Mapper(const NetworkTickAlignmentPolicy policy = {20, 5, 4}) {
            return NetworkTickAlignment::Create(Connection(), Session(), policy).Value();
        }

        [[nodiscard]] NetworkClockSample Sample(const NetworkTickAlignment &mapper, const std::uint64_t sequence,
                                                const std::uint64_t localTick, const std::uint64_t serverTick,
                                                const std::uint64_t roundTripTicks = 0) {
            const auto snapshot = mapper.Snapshot();
            return {snapshot.connection, snapshot.session, snapshot.clockEpoch, sequence, localTick, serverTick, roundTripTicks};
        }
    }  // namespace

    TEST_CASE("Network tick alignment validates policy and requires a first current-epoch sample", "[network][clock]") {
        RequireError(NetworkTickAlignment::Create({}, Session(), {20, 5, 4}), NetworkErrors::NetworkClockInvalid);
        RequireError(NetworkTickAlignment::Create(Connection(), {}, {20, 5, 4}), NetworkErrors::NetworkClockInvalid);
        RequireError(NetworkTickAlignment::Create(Connection(), Session(), {0, 5, 4}), NetworkErrors::NetworkClockInvalid);
        RequireError(NetworkTickAlignment::Create(Connection(), Session(), {20, 0, 4}), NetworkErrors::NetworkClockInvalid);
        RequireError(NetworkTickAlignment::Create(Connection(), Session(), {20, 5, 0}), NetworkErrors::NetworkClockInvalid);
        RequireError(NetworkTickAlignment::Create(Connection(), Session(), {20, 5, MaximumNetworkClockSamples + 1}),
                     NetworkErrors::NetworkClockInvalid);

        auto mapper = Mapper();
        RequireError(mapper.Advance(1), NetworkErrors::NetworkClockUnavailable);
        REQUIRE_FALSE(mapper.Snapshot().hasMapping);
        REQUIRE(mapper.Observe(Sample(mapper, 1, 10, 20, 4)).HasValue());
        const auto anchored = mapper.Snapshot();
        REQUIRE(anchored.hasMapping);
        REQUIRE(anchored.localTick == 10);
        REQUIRE(anchored.serverTick == 22);
        REQUIRE(anchored.quality == NetworkTimingQuality::Tracking);
        RequireError(mapper.Advance(12), NetworkErrors::NetworkClockInvalid);
        REQUIRE(mapper.Advance(11).Value().serverTick == 23);
    }

    TEST_CASE("Clock correction and malformed traffic cannot amplify fixed-step catch-up", "[network][clock]") {
        auto mapper = Mapper({20, 2000, 1});
        REQUIRE(mapper.Observe(Sample(mapper, 1, 0, 0)).HasValue());
        REQUIRE(mapper.Advance(1).Value().serverTick == 1);
        REQUIRE(mapper.Observe(Sample(mapper, 2, 1, 1000)).HasValue());

        std::uint64_t previous = mapper.Snapshot().serverTick;
        for (std::uint64_t localTick = 2; localTick <= 1000; ++localTick) {
            const auto mapped = mapper.Advance(localTick);
            REQUIRE(mapped.HasValue());
            REQUIRE(mapped.Value().serverTick >= previous);
            REQUIRE(mapped.Value().serverTick - previous <= 2);
            previous = mapped.Value().serverTick;
        }
        REQUIRE(previous <= 1999);
        REQUIRE(mapper.Snapshot().retainedSamples == 1);
        RequireError(mapper.Advance(1000), NetworkErrors::NetworkClockInvalid);
    }

    TEST_CASE("Fixed history exposes drift and loss quality without losing monotonic mapping", "[network][clock]") {
        auto mapper = Mapper({20, 2, 2});
        REQUIRE(mapper.Observe(Sample(mapper, 1, 10, 20, 4)).HasValue());
        REQUIRE(mapper.Advance(11).Value().quality == NetworkTimingQuality::Holdover);
        REQUIRE(mapper.Observe(Sample(mapper, 2, 11, 24)).HasValue());
        const auto measured = mapper.Snapshot();
        REQUIRE(measured.retainedSamples == 2);
        REQUIRE(measured.driftWindowTicks == 1);
        REQUIRE(measured.driftMagnitudeTicks == 1);
        REQUIRE(measured.serverClockAhead);
        REQUIRE(mapper.Advance(12).Value().serverTick >= measured.serverTick);
        REQUIRE(mapper.Advance(13).Value().quality == NetworkTimingQuality::Holdover);
        const auto stale = mapper.Advance(14).Value();
        REQUIRE(stale.quality == NetworkTimingQuality::Stale);
        REQUIRE(mapper.Advance(15).Value().serverTick == stale.serverTick + 1);
    }

    TEST_CASE("A slower server estimate may hold but never rewind the projected tick", "[network][clock]") {
        auto mapper = Mapper({20, 10, 1});
        REQUIRE(mapper.Observe(Sample(mapper, 1, 10, 20)).HasValue());
        for (std::uint64_t localTick = 11; localTick <= 15; ++localTick)
            REQUIRE(mapper.Advance(localTick).HasValue());
        REQUIRE(mapper.Snapshot().serverTick == 25);
        REQUIRE(mapper.Observe(Sample(mapper, 2, 15, 21)).HasValue());
        RequireError(mapper.Observe(Sample(mapper, 3, 15, 22)), NetworkErrors::NetworkClockSampleStale);
        REQUIRE(mapper.Advance(16).Value().serverTick == 25);
        REQUIRE(mapper.Advance(17).Value().serverTick == 25);
    }

    TEST_CASE("Sample history remains capped and evicts the oldest measurement", "[network][clock]") {
        auto mapper = Mapper({20, 10, 2});
        REQUIRE(mapper.Observe(Sample(mapper, 1, 0, 1)).HasValue());
        REQUIRE(mapper.Advance(1).HasValue());
        REQUIRE(mapper.Observe(Sample(mapper, 2, 1, 3)).HasValue());
        REQUIRE(mapper.Advance(2).HasValue());
        REQUIRE(mapper.Observe(Sample(mapper, 3, 2, 5)).HasValue());
        const auto snapshot = mapper.Snapshot();
        REQUIRE(snapshot.retainedSamples == 2);
        REQUIRE(snapshot.driftWindowTicks == 1);
        REQUIRE(snapshot.driftMagnitudeTicks == 1);
    }

    TEST_CASE("Old samples and invalid round trips leave the current mapping unchanged", "[network][clock]") {
        auto mapper = Mapper();
        REQUIRE(mapper.Observe(Sample(mapper, 1, 10, 20)).HasValue());
        const auto baseline = mapper.Snapshot();
        RequireError(mapper.Observe(Sample(mapper, 1, 10, 21)), NetworkErrors::NetworkClockSampleStale);
        RequireError(mapper.Observe(Sample(mapper, 2, 10, 20)), NetworkErrors::NetworkClockSampleStale);
        auto foreign = Sample(mapper, 2, 10, 21);
        foreign.session = Session(8);
        RequireError(mapper.Observe(foreign), NetworkErrors::NetworkClockSampleStale);
        foreign = Sample(mapper, 2, 10, 21);
        foreign.connection = Connection(2);
        RequireError(mapper.Observe(foreign), NetworkErrors::NetworkClockSampleStale);
        RequireError(mapper.Observe(Sample(mapper, 2, 10, 21, 21)), NetworkErrors::NetworkClockInvalid);
        RequireError(mapper.Observe(Sample(mapper, 2, 9, 21)), NetworkErrors::NetworkClockSampleStale);
        REQUIRE(mapper.Snapshot().serverTick == baseline.serverTick);
        REQUIRE(mapper.Snapshot().retainedSamples == baseline.retainedSamples);
        REQUIRE(mapper.Advance(11).HasValue());
        RequireError(mapper.Observe(Sample(mapper, 2, 11, 20)), NetworkErrors::NetworkClockSampleStale);
        RequireError(mapper.Observe(Sample(mapper, 2, 12, 21)), NetworkErrors::NetworkClockInvalid);
    }

    TEST_CASE("Pause suspend and reconnect fence late clock responses", "[network][clock]") {
        auto mapper = Mapper();
        REQUIRE(mapper.Observe(Sample(mapper, 1, 10, 20)).HasValue());
        const auto old = Sample(mapper, 2, 10, 21);
        const auto firstEpoch = mapper.Snapshot().clockEpoch;
        REQUIRE(mapper.Pause().HasValue());
        REQUIRE(mapper.Snapshot().state == NetworkTickAlignmentState::Paused);
        REQUIRE(mapper.Snapshot().quality == NetworkTimingQuality::Acquiring);
        REQUIRE(mapper.Snapshot().clockEpoch == firstEpoch + 1);
        RequireError(mapper.Advance(11), NetworkErrors::NetworkClockUnavailable);
        REQUIRE(mapper.Resume().HasValue());
        REQUIRE(mapper.Snapshot().quality == NetworkTimingQuality::Acquiring);
        RequireError(mapper.Observe(old), NetworkErrors::NetworkClockSampleStale);
        REQUIRE(mapper.Advance(11).Value().serverTick == 21);

        REQUIRE(mapper.Pause().HasValue());
        REQUIRE(mapper.Suspend().HasValue());
        REQUIRE(mapper.Snapshot().state == NetworkTickAlignmentState::Suspended);
        REQUIRE(mapper.Snapshot().quality == NetworkTimingQuality::Acquiring);
        RequireError(mapper.Advance(12), NetworkErrors::NetworkClockUnavailable);
        REQUIRE(mapper.ResumeFromSuspend().HasValue());
        REQUIRE(mapper.Snapshot().state == NetworkTickAlignmentState::Paused);
        REQUIRE(mapper.Resume().HasValue());
        REQUIRE(mapper.Observe(Sample(mapper, 2, 11, 30)).HasValue());
        REQUIRE(mapper.Snapshot().quality == NetworkTimingQuality::Tracking);
        REQUIRE(mapper.Advance(12).Value().serverTick == 23);

        REQUIRE(mapper.Disconnect().HasValue());
        REQUIRE(mapper.Snapshot().state == NetworkTickAlignmentState::Disconnected);
        RequireError(mapper.Observe(Sample(mapper, 3, 12, 31)), NetworkErrors::NetworkClockUnavailable);
        RequireError(mapper.Replace(Connection(3), Session(8)), NetworkErrors::NetworkClockSampleStale);
        RequireError(mapper.Replace(Connection(2), Session()), NetworkErrors::NetworkClockSampleStale);
        REQUIRE(mapper.Replace(Connection(2), Session(8)).HasValue());
        REQUIRE_FALSE(mapper.Snapshot().hasMapping);
        REQUIRE(mapper.Snapshot().quality == NetworkTimingQuality::Acquiring);
        RequireError(mapper.Observe(old), NetworkErrors::NetworkClockSampleStale);
        REQUIRE(mapper.Observe(Sample(mapper, 1, 0, 4)).HasValue());
        REQUIRE(mapper.Advance(1).Value().serverTick == 5);
        mapper.Shutdown();
        mapper.Shutdown();
        RequireError(mapper.Advance(2), NetworkErrors::NetworkClockUnavailable);
        RequireError(mapper.Replace(Connection(3), Session(9)), NetworkErrors::NetworkClockSampleStale);
    }

    TEST_CASE("Reconnect may use another admitted slot or a new session on the same connection", "[network][clock]") {
        auto sameConnection = Mapper();
        REQUIRE(sameConnection.Observe(Sample(sameConnection, 1, 4, 8)).HasValue());
        REQUIRE(sameConnection.Disconnect().HasValue());
        REQUIRE(sameConnection.Replace(Connection(), Session(8)).HasValue());
        REQUIRE_FALSE(sameConnection.Snapshot().hasMapping);

        auto otherSlot = Mapper();
        REQUIRE(otherSlot.Disconnect().HasValue());
        const auto newConnection = ConnectionHandle::Create(4, 1).Value();
        REQUIRE(otherSlot.Replace(newConnection, Session(8)).HasValue());
        REQUIRE(otherSlot.Snapshot().connection == newConnection);

        auto exhaustedSlot =
            NetworkTickAlignment::Create(Connection(std::numeric_limits<std::uint32_t>::max()), Session(), {20, 5, 4}).Value();
        REQUIRE(exhaustedSlot.Disconnect().HasValue());
        RequireError(exhaustedSlot.Replace(Connection(1), Session(8)), NetworkErrors::NetworkClockSampleStale);
        REQUIRE(exhaustedSlot.Replace(newConnection, Session(8)).HasValue());
    }

    TEST_CASE("Active suspension resumes at the next committed tick without wall-time catch-up", "[network][clock]") {
        auto mapper = Mapper();
        REQUIRE(mapper.Observe(Sample(mapper, 1, 20, 40)).HasValue());
        const auto pending = Sample(mapper, 2, 20, 41);
        REQUIRE(mapper.Suspend().HasValue());
        REQUIRE(mapper.Snapshot().state == NetworkTickAlignmentState::Suspended);
        RequireError(mapper.Advance(21), NetworkErrors::NetworkClockUnavailable);
        RequireError(mapper.Observe(pending), NetworkErrors::NetworkClockUnavailable);
        REQUIRE(mapper.ResumeFromSuspend().HasValue());
        REQUIRE(mapper.Snapshot().state == NetworkTickAlignmentState::Tracking);
        RequireError(mapper.Observe(pending), NetworkErrors::NetworkClockSampleStale);
        RequireError(mapper.Advance(22), NetworkErrors::NetworkClockInvalid);
        REQUIRE(mapper.Advance(21).Value().serverTick == 41);
        REQUIRE(mapper.Snapshot().quality == NetworkTimingQuality::Acquiring);
        REQUIRE(mapper.Observe(Sample(mapper, 2, 21, 42)).HasValue());
        REQUIRE(mapper.Snapshot().quality == NetworkTimingQuality::Tracking);
    }

    TEST_CASE("Tick and round-trip overflow fail without publishing wrapped mapping", "[network][clock]") {
        auto mapper = Mapper();
        RequireError(mapper.Observe(Sample(mapper, 1, 0, std::numeric_limits<std::uint64_t>::max(), 2)),
                     NetworkErrors::NetworkClockOverflow);
        REQUIRE_FALSE(mapper.Snapshot().hasMapping);
        REQUIRE(mapper.Observe(Sample(mapper, 1, 0, std::numeric_limits<std::uint64_t>::max())).HasValue());
        RequireError(mapper.Advance(1), NetworkErrors::NetworkClockOverflow);
        REQUIRE(mapper.Snapshot().localTick == 0);
        REQUIRE(mapper.Snapshot().serverTick == std::numeric_limits<std::uint64_t>::max());

        auto localOverflow = Mapper();
        constexpr std::uint64_t last = std::numeric_limits<std::uint64_t>::max();
        REQUIRE(localOverflow.Observe(Sample(localOverflow, 1, last - 1, 1)).HasValue());
        REQUIRE(localOverflow.Advance(last).HasValue());
        RequireError(localOverflow.Advance(last), NetworkErrors::NetworkClockOverflow);
    }
}  // namespace Horo::Network
