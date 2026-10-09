#include "Horo/Network/ReplicationSnapshotHistory.h"
#include "ReplicationStateCodecTestSupport.h"

#include <limits>

namespace Horo::Network {
    using namespace StateCodecTestSupport;

    namespace {
        ReplicationHistoryScope Scope() {
            return {ConnectionHandle::Create(1, 1).Value(), World().session, World().scene, 1};
        }

        std::unique_ptr<ReplicationSnapshotHistory> History(const ReplicationHistoryLimits limits = {}) {
            auto result = ReplicationSnapshotHistory::Create(Scope(), limits);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        ReplicationAcknowledgedBaseline Select(ReplicationSnapshotHistory &history, CodecFixture &fixture, const std::uint64_t now) {
            auto result = history.Baseline(fixture.capture.Pin(), Recipient().revision, fixture.codec->DescriptorGeneration(),
                                           fixture.codec->ProjectionFingerprint(), now);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    }  // namespace

    TEST_CASE("Connection history uses only selectively acknowledged exact projections", "[network][snapshot-history]") {
        CodecFixture fixture;
        auto history = History();
        const auto first = history->RetainSent(Ack(fixture.first, *fixture.codec), 1).Value();
        REQUIRE_FALSE(Select(*history, fixture, 1).state);
        fixture.capture.Capture(2, 2.0);
        const auto second = history->RetainSent(Ack(fixture.capture.Pin(), *fixture.codec), 2).Value();
        REQUIRE(history->Acknowledge(second, 2).HasValue());
        REQUIRE(Select(*history, fixture, 2).publicationRevision == second.publicationRevision);
        REQUIRE(history->Acknowledge(second, 3).HasValue());
        REQUIRE(history->Acknowledge(first, 3).HasValue());
        REQUIRE(Select(*history, fixture, 3).publicationRevision == second.publicationRevision);
        REQUIRE(history->Baseline(fixture.first, Recipient().revision, 1, fixture.codec->ProjectionFingerprint(), 3).Value().state ==
                fixture.first);
        const auto delta = fixture.codec->Encode(fixture.capture.Pin(), Select(*history, fixture, 3)).Value();
        REQUIRE(delta[5] == std::byte{1});
        REQUIRE_FALSE(
            history
                ->Baseline(fixture.capture.Pin(), ReplicationRoleRevision::Create(2).Value(), 1, fixture.codec->ProjectionFingerprint(), 3)
                .Value()
                .state);
        REQUIRE_FALSE(
            history->Baseline(fixture.capture.Pin(), Recipient().revision, 2, fixture.codec->ProjectionFingerprint(), 3).Value().state);
        REQUIRE_FALSE(history->Baseline(fixture.capture.Pin(), Recipient().revision, 1, {}, 3).Value().state);
    }

    TEST_CASE("Lost acknowledgement and fixed lease expiry recover with complete full state", "[network][snapshot-history]") {
        CodecFixture fixture;
        auto history = History({4, 4096, 3});
        const auto pins = fixture.first.use_count();
        const auto token = history->RetainSent(Ack(fixture.first, *fixture.codec), 1).Value();
        REQUIRE(fixture.first.use_count() == pins + 1);
        SECTION("ack lost") {
            REQUIRE_FALSE(Select(*history, fixture, 3).state);
        }
        SECTION("duplicate does not extend lease") {
            REQUIRE(history->Acknowledge(token, 2).HasValue());
            REQUIRE(history->Acknowledge(token, 3).HasValue());
            REQUIRE(Select(*history, fixture, 3).state);
        }
        REQUIRE(history->Expire(4).HasValue());
        REQUIRE(fixture.first.use_count() == pins);
        REQUIRE_FALSE(Select(*history, fixture, 4).state);
        REQUIRE(history->Acknowledge(token, 4).HasError());
        const auto wire = fixture.codec->Encode(fixture.capture.Pin(), Select(*history, fixture, 4)).Value();
        REQUIRE(wire[5] == std::byte{0});
        REQUIRE(fixture.codec->Decode(wire, Object()).HasValue());
    }

    TEST_CASE("Old session and connection incarnations cannot select or release current history", "[network][snapshot-history]") {
        CodecFixture fixture;
        auto history = History({4, 4096, 3});
        auto token = history->RetainSent(Ack(fixture.first, *fixture.codec), 1).Value();
        const auto original = token;
        SECTION("session") {
            token.scope.session = NetworkSessionGeneration::Create(2).Value();
        }
        SECTION("connection generation") {
            token.scope.connection = ConnectionHandle::Create(1, 2).Value();
        }
        SECTION("scene") {
            token.scope.scene = Runtime::SceneRuntimeId{11};
        }
        SECTION("history incarnation") {
            ++token.scope.incarnation;
        }
        SECTION("object occurrence") {
            token.object = Object(1, 2).object;
        }
        SECTION("publication") {
            ++token.publicationRevision;
        }
        // Foreign scope rejection must not advance the clock or expire current entries.
        REQUIRE(history->Acknowledge(token, token.scope == Scope() ? 2 : 100).HasError());
        REQUIRE(history->Acknowledge(original, 2).HasValue());
        REQUIRE(Select(*history, fixture, 2).state == fixture.first);
    }

    TEST_CASE("History overflow and memory pressure release pins without reusing identities", "[network][snapshot-history]") {
        CodecFixture fixture;
        auto history = History({1, 4096, 10});
        const auto token = history->RetainSent(Ack(fixture.first, *fixture.codec), 1).Value();
        REQUIRE(history->Acknowledge(token, 1).HasValue());
        CaptureTestSupport::RequireError(history->RetainSent(Ack(fixture.first, *fixture.codec), 2), ReplicationStateErrors::Capacity);
        REQUIRE_FALSE(Select(*history, fixture, 2).state);
        const auto fresh = history->RetainSent(Ack(fixture.first, *fixture.codec), 2).Value();
        REQUIRE(fresh.sequence > token.sequence);
        CaptureTestSupport::RequireError(history->Acknowledge(token, 2), ReplicationStateErrors::Stale);
        REQUIRE(history->Acknowledge(fresh, 2).HasValue());
        REQUIRE(history->ReleaseForMemoryPressure().HasValue());
        REQUIRE_FALSE(Select(*history, fixture, 2).state);
        REQUIRE(history->Acknowledge(fresh, 2).HasError());
        auto disconnect = History({1, 4096, 10, ReplicationHistoryOverflow::Disconnect});
        REQUIRE(disconnect->RetainSent(Ack(fixture.first, *fixture.codec), 1).HasValue());
        CaptureTestSupport::RequireError(disconnect->RetainSent(Ack(fixture.first, *fixture.codec), 1), ReplicationStateErrors::Capacity);
        CaptureTestSupport::RequireError(disconnect->RetainSent(Ack(fixture.first, *fixture.codec), 1), ReplicationStateErrors::Closed);
    }

    TEST_CASE("History cancellation clock bounds revocation and shutdown preserve lifetime safety", "[network][snapshot-history]") {
        CodecFixture fixture;
        auto history = History();
        const auto token = history->RetainSent(Ack(fixture.first, *fixture.codec), 2).Value();
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        CaptureTestSupport::RequireError(history->Acknowledge(token, 1000, cancellation.Token()), NetworkErrors::ReplicationWorldCancelled);
        REQUIRE(history->RetainSent(Ack(fixture.first, *fixture.codec), 1000, cancellation.Token()).HasError());
        REQUIRE(
            history->Baseline(fixture.first, Recipient().revision, 1, fixture.codec->ProjectionFingerprint(), 1000, cancellation.Token())
                .HasError());
        REQUIRE(history->Acknowledge(token, 1).HasError());
        REQUIRE(history->Acknowledge(token, 2).HasValue());
        SECTION("revoked capture") {
            fixture.capture.capture->Shutdown();
            REQUIRE(history->Acknowledge(token, 2).HasError());
            REQUIRE(history->Baseline(fixture.first, Recipient().revision, 1, fixture.codec->ProjectionFingerprint(), 2).HasError());
        }
        SECTION("lease clock exhausted") {
            REQUIRE(history->RetainSent(Ack(fixture.first, *fixture.codec), std::numeric_limits<std::uint64_t>::max()).HasError());
        }
        REQUIRE(history->Shutdown().HasValue());
        REQUIRE(history->Shutdown().HasValue());
        REQUIRE(history->Acknowledge(token, 2).HasError());
        REQUIRE(history->RetainSent(Ack(fixture.first, *fixture.codec), 2).HasError());
        REQUIRE(std::get<double>(fixture.first->Fields()[0].value) == 1.0);
    }

    TEST_CASE("History rejects malformed setup oversized backing and foreign-thread mutation", "[network][snapshot-history]") {
        REQUIRE(ReplicationSnapshotHistory::Create({}).HasError());
        REQUIRE(ReplicationSnapshotHistory::Create(Scope(), {0, 4096, 1}).HasError());
        REQUIRE(ReplicationSnapshotHistory::Create(Scope(), {4097, 4096, 1}).HasError());
        REQUIRE(ReplicationSnapshotHistory::Create(Scope(), {1, 0, 1}).HasError());
        REQUIRE(ReplicationSnapshotHistory::Create(Scope(), {1, 4096, 0}).HasError());
        CodecFixture fixture;
        auto tiny = History({1, 1, 1});
        REQUIRE(tiny->RetainSent(Ack(fixture.first, *fixture.codec), 1).HasError());
        auto history = History();
        bool rejected{};
        std::thread thread{[&] {
            rejected = history->RetainSent(Ack(fixture.first, *fixture.codec), 1).HasError();
        }};
        thread.join();
        REQUIRE(rejected);
        REQUIRE_FALSE(Select(*history, fixture, 1).state);
    }

    TEST_CASE("Connection isolation and replacement release only the retiring owner's pins", "[network][snapshot-history]") {
        CodecFixture fixture;
        auto first = History();
        auto scope = Scope();
        scope.connection = ConnectionHandle::Create(2, 1).Value();
        auto second = ReplicationSnapshotHistory::Create(scope).Value();
        const auto initialPins = fixture.first.use_count();
        const auto a = first->RetainSent(Ack(fixture.first, *fixture.codec), 1).Value();
        const auto b = second->RetainSent(Ack(fixture.first, *fixture.codec), 1).Value();
        REQUIRE(fixture.first.use_count() == initialPins + 2);
        REQUIRE(second->Acknowledge(a, 1000).HasError());
        REQUIRE(second->Acknowledge(b, 1).HasValue());
        REQUIRE_FALSE(Select(*first, fixture, 1).state);
        REQUIRE(Select(*second, fixture, 1).state);
        REQUIRE(first->Shutdown().HasValue());
        REQUIRE(fixture.first.use_count() == initialPins + 1);
        scope = Scope();
        ++scope.incarnation;
        first = ReplicationSnapshotHistory::Create(scope).Value();
        const auto replacement = first->RetainSent(Ack(fixture.first, *fixture.codec), 1).Value();
        REQUIRE(replacement.sequence == a.sequence);
        REQUIRE(first->Acknowledge(a, 1).HasError());
        REQUIRE_FALSE(Select(*first, fixture, 1).state);
        REQUIRE(first->Acknowledge(replacement, 1).HasValue());
        REQUIRE(second->Shutdown().HasValue());
        REQUIRE(fixture.first.use_count() == initialPins + 1);
        first.reset();
        REQUIRE(fixture.first.use_count() == initialPins);
    }

    TEST_CASE("Cancelled send submission releases only its reservation and never accepts delayed receipt", "[network][snapshot-history]") {
        CodecFixture fixture;
        auto history = History();
        const auto initialPins = fixture.first.use_count();
        const auto accepted = history->RetainSent(Ack(fixture.first, *fixture.codec), 1).Value();
        const auto cancelled = history->RetainSent(Ack(fixture.first, *fixture.codec), 1).Value();
        REQUIRE(history->Acknowledge(accepted, 1).HasValue());
        auto foreign = cancelled;
        ++foreign.scope.incarnation;
        REQUIRE(history->CancelSent(foreign, 1000).HasError());
        REQUIRE(history->CancelSent(cancelled, 1).HasValue());
        REQUIRE(fixture.first.use_count() == initialPins + 1);
        REQUIRE(history->Acknowledge(cancelled, 1).HasError());
        REQUIRE(Select(*history, fixture, 1).state);
        const auto next = history->RetainSent(Ack(fixture.first, *fixture.codec), 1).Value();
        REQUIRE(next.sequence > cancelled.sequence);
    }
}  // namespace Horo::Network
