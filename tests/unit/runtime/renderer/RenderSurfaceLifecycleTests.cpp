#include "Horo/Runtime/Render/RenderSurfaceLifecycle.h"
#include "Horo/Runtime/Render/RenderSurfaceLifecycleErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace Horo::Render::SurfaceLifecycleTests {

    [[nodiscard]] RenderSurfaceConfiguration Configuration(const std::uint32_t width = 1280, const std::uint64_t displayRevision = 1) {
        return {.extent = {width, 720},
                .presentMode = {PresentMode::Fifo, PresentMode::Fifo, PresentModeResolution::Exact, 0},
                .displayRevision = displayRevision};
    }

    [[nodiscard]] RenderSurfaceCommand Command(const std::uint64_t sequence, const RenderSurfaceCommandKind kind,
                                               std::optional<RenderSurfaceConfiguration> target = std::nullopt) {
        return {sequence, kind, std::move(target)};
    }

    [[nodiscard]] bool HasCode(const Error &error, const ErrorCodeDescriptor &descriptor) {
        return std::pair{error.domain.Value(), error.code.Value()} == std::pair{descriptor.domain.Value(), descriptor.code.Value()};
    }

    RenderSurfaceSnapshot Complete(RenderSurfaceLifecycle &lifecycle, const RenderSurfaceRealization realization) {
        const auto transition = lifecycle.BeginFrameBoundary();
        REQUIRE(transition.HasValue());
        const auto completed = lifecycle.Complete(transition.Value(), realization);
        REQUIRE(completed.HasValue());
        return completed.Value();
    }

    RenderSurfaceLifecycle Lifecycle(const std::uint64_t owner = 42) {
        auto created = RenderSurfaceLifecycle::Create(owner);
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    void QueueResize(RenderSurfaceLifecycle &lifecycle, const std::uint64_t sequence, const std::uint32_t width,
                     const std::uint64_t displayRevision) {
        const auto queued = lifecycle.Queue(Command(sequence, RenderSurfaceCommandKind::Resize, Configuration(width, displayRevision)));
        REQUIRE(queued.HasValue());
    }

    RenderSurfaceTransition FreezeResize(RenderSurfaceLifecycle &lifecycle, const std::uint64_t sequence, const std::uint32_t width,
                                         const std::uint64_t displayRevision) {
        QueueResize(lifecycle, sequence, width, displayRevision);
        const auto transition = lifecycle.BeginFrameBoundary();
        REQUIRE(transition.HasValue());
        return transition.Value();
    }

    RenderSurfaceLifecycle ReadyLifecycle() {
        RenderSurfaceLifecycle lifecycle = Lifecycle();
        REQUIRE(lifecycle.Queue(Command(1, RenderSurfaceCommandKind::Attach, Configuration())).HasValue());
        const RenderSurfaceSnapshot ready = Complete(lifecycle, RenderSurfaceRealization::Ready);
        REQUIRE(ready.state == RenderSurfaceState::Ready);
        return lifecycle;
    }

    TEST_CASE("Surface attachment publishes only an exact completed candidate", "[runtime][renderer][surface]") {
        RenderSurfaceLifecycle lifecycle = Lifecycle();
        const auto initial = lifecycle.Snapshot();
        REQUIRE(initial.HasValue());
        CHECK(initial.Value().surface == RenderSurfaceId{42, 0});
        CHECK(initial.Value().revision == 1);
        CHECK(initial.Value().state == RenderSurfaceState::Unattached);

        const auto queued = lifecycle.Queue(Command(1, RenderSurfaceCommandKind::Attach, Configuration()));
        REQUIRE(queued.HasValue());
        CHECK(queued.Value().disposition == RenderSurfaceQueueDisposition::Accepted);
        const auto admitted = lifecycle.Snapshot();
        REQUIRE(admitted.HasValue());
        CHECK(admitted.Value().revision == 2);
        CHECK(admitted.Value().state == RenderSurfaceState::Unattached);
        CHECK(admitted.Value().pendingCandidate == Command(1, RenderSurfaceCommandKind::Attach, Configuration()));
        CHECK_FALSE(admitted.Value().inFlightSequence.has_value());
        const auto transition = lifecycle.BeginFrameBoundary();
        REQUIRE(transition.HasValue());
        CHECK(transition.Value().sourceSurface == RenderSurfaceId{42, 0});
        CHECK(transition.Value().sourceRevision == 2);
        const auto pending = lifecycle.Snapshot();
        REQUIRE(pending.HasValue());
        CHECK(pending.Value().state == RenderSurfaceState::Reconfiguring);
        CHECK_FALSE(pending.Value().pendingCandidate.has_value());
        CHECK(pending.Value().inFlightSequence == 1);
        REQUIRE(lifecycle.Queue(Command(2, RenderSurfaceCommandKind::Close)).HasValue());
        CHECK(lifecycle.HasPendingRequest().Value());
        const auto queuedClose = lifecycle.Snapshot();
        REQUIRE(queuedClose.HasValue());
        CHECK(queuedClose.Value().pendingCandidate == Command(2, RenderSurfaceCommandKind::Close));
        CHECK(queuedClose.Value().inFlightSequence == 1);

        const auto complete = lifecycle.Complete(transition.Value(), RenderSurfaceRealization::Ready);
        REQUIRE(complete.HasValue());
        CHECK(complete.Value().surface == RenderSurfaceId{42, 1});
        CHECK(complete.Value().revision == 5);
        CHECK(complete.Value().state == RenderSurfaceState::Ready);
        CHECK(complete.Value().active == Configuration());
        CHECK(complete.Value().pendingCandidate == Command(2, RenderSurfaceCommandKind::Close));
        CHECK_FALSE(complete.Value().inFlightSequence.has_value());
        CHECK(Complete(lifecycle, RenderSurfaceRealization::Unattached).state == RenderSurfaceState::Unattached);
    }

    TEST_CASE("Surface requests coalesce with explicit supersession and close priority", "[runtime][renderer][surface]") {
        RenderSurfaceLifecycle lifecycle = ReadyLifecycle();
        QueueResize(lifecycle, 2, 1600, 2);
        const auto coalesced = lifecycle.Queue(Command(3, RenderSurfaceCommandKind::Resize, Configuration(1920, 3)));
        REQUIRE(coalesced.HasValue());
        CHECK(coalesced.Value() == RenderSurfaceQueueResult{RenderSurfaceQueueDisposition::Coalesced, 2});
        const auto coalescedSnapshot = lifecycle.Snapshot();
        REQUIRE(coalescedSnapshot.HasValue());
        CHECK(coalescedSnapshot.Value().pendingCandidate == Command(3, RenderSurfaceCommandKind::Resize, Configuration(1920, 3)));

        const auto close = lifecycle.Queue(Command(4, RenderSurfaceCommandKind::Close));
        REQUIRE(close.HasValue());
        CHECK(close.Value() == RenderSurfaceQueueResult{RenderSurfaceQueueDisposition::Coalesced, 3});
        const auto rejected = lifecycle.Queue(Command(5, RenderSurfaceCommandKind::Resize, Configuration(2560, 4)));
        REQUIRE(rejected.HasError());
        CHECK(HasCode(rejected.ErrorValue(), RenderSurfaceLifecycleErrors::InvalidState));

        const RenderSurfaceSnapshot closed = Complete(lifecycle, RenderSurfaceRealization::Unattached);
        CHECK(closed.state == RenderSurfaceState::Unattached);
        CHECK_FALSE(closed.active.has_value());
        CHECK_FALSE(lifecycle.HasPendingRequest().Value());
    }

    TEST_CASE("Surface requests arriving during realization wait without mutating the frozen candidate", "[runtime][renderer][surface]") {
        RenderSurfaceLifecycle lifecycle = ReadyLifecycle();
        const RenderSurfaceTransition frozen = FreezeResize(lifecycle, 2, 1600, 2);
        REQUIRE(lifecycle.Queue(Command(3, RenderSurfaceCommandKind::Replace, Configuration(1920, 3))).HasValue());
        CHECK(lifecycle.HasPendingRequest().Value());
        const auto queuedDuringRealization = lifecycle.Snapshot();
        REQUIRE(queuedDuringRealization.HasValue());
        CHECK(queuedDuringRealization.Value().pendingCandidate == Command(3, RenderSurfaceCommandKind::Replace, Configuration(1920, 3)));
        CHECK(queuedDuringRealization.Value().inFlightSequence == 2);

        const auto first = lifecycle.Complete(frozen, RenderSurfaceRealization::Ready);
        REQUIRE(first.HasValue());
        CHECK(first.Value().active == Configuration(1600, 2));
        const auto next = lifecycle.BeginFrameBoundary();
        REQUIRE(next.HasValue());
        CHECK(next.Value().command == Command(3, RenderSurfaceCommandKind::Replace, Configuration(1920, 3)));
        const auto second = lifecycle.Complete(next.Value(), RenderSurfaceRealization::Ready);
        REQUIRE(second.HasValue());
        CHECK(second.Value().active == Configuration(1920, 3));
    }

    TEST_CASE("Prior native loss explicitly invalidates an incompatible queued candidate", "[runtime][renderer][surface]") {
        RenderSurfaceLifecycle lifecycle = ReadyLifecycle();
        QueueResize(lifecycle, 2, 1600, 2);
        const auto frozenResult = lifecycle.BeginFrameBoundary();
        REQUIRE(frozenResult.HasValue());
        const RenderSurfaceTransition frozen = frozenResult.Value();
        REQUIRE(lifecycle.Queue(Command(3, RenderSurfaceCommandKind::Suspend)).HasValue());
        const auto lost = lifecycle.Complete(frozen, RenderSurfaceRealization::Lost);
        REQUIRE(lost.HasValue());
        CHECK(lost.Value().state == RenderSurfaceState::Lost);

        const auto invalidated = lifecycle.BeginFrameBoundary();
        REQUIRE(invalidated.HasError());
        CHECK(HasCode(invalidated.ErrorValue(), RenderSurfaceLifecycleErrors::PendingRequestInvalidated));
        CHECK(invalidated.ErrorValue().message.find("sequence 3") != std::string::npos);
        CHECK_FALSE(lifecycle.HasPendingRequest().Value());
        CHECK_FALSE(lifecycle.Snapshot().Value().pendingCandidate.has_value());

        REQUIRE(lifecycle.Queue(Command(4, RenderSurfaceCommandKind::Recover, Configuration(1600, 3))).HasValue());
        CHECK(Complete(lifecycle, RenderSurfaceRealization::Ready).state == RenderSurfaceState::Ready);
    }

    TEST_CASE("Surface lifecycle preserves usable output and reports suspended or lost output honestly", "[runtime][renderer][surface]") {
        RenderSurfaceLifecycle lifecycle = ReadyLifecycle();
        const RenderSurfaceConfiguration original = *lifecycle.Snapshot().Value().active;

        QueueResize(lifecycle, 2, 1600, 2);
        const RenderSurfaceSnapshot preserved = Complete(lifecycle, RenderSurfaceRealization::PreserveActive);
        CHECK(preserved.state == RenderSurfaceState::Ready);
        CHECK(preserved.surface.generation == 1);
        CHECK(preserved.active == original);

        REQUIRE(lifecycle.Queue(Command(3, RenderSurfaceCommandKind::Suspend)).HasValue());
        const RenderSurfaceSnapshot suspended = Complete(lifecycle, RenderSurfaceRealization::Suspended);
        CHECK(suspended.state == RenderSurfaceState::Suspended);
        CHECK(suspended.active == original);

        REQUIRE(lifecycle.Queue(Command(4, RenderSurfaceCommandKind::Replace, Configuration(1920, 3))).HasValue());
        const RenderSurfaceSnapshot lost = Complete(lifecycle, RenderSurfaceRealization::Lost);
        CHECK(lost.state == RenderSurfaceState::Lost);
        CHECK_FALSE(lost.active.has_value());
        REQUIRE(lifecycle.Queue(Command(5, RenderSurfaceCommandKind::Recover, Configuration(1920, 4))).HasValue());
        const RenderSurfaceSnapshot recovered = Complete(lifecycle, RenderSurfaceRealization::Ready);
        CHECK(recovered.state == RenderSurfaceState::Ready);
        CHECK(recovered.surface.generation == 2);
        CHECK(recovered.active == Configuration(1920, 4));
    }

    TEST_CASE("Surface lifecycle rejects malformed stale mismatched and overlapping work", "[runtime][renderer][surface]") {
        const auto invalidOwner = RenderSurfaceLifecycle::Create(0);
        REQUIRE(invalidOwner.HasError());
        CHECK(HasCode(invalidOwner.ErrorValue(), RenderSurfaceLifecycleErrors::InvalidIdentity));

        RenderSurfaceLifecycle empty = Lifecycle(7);
        const auto noPending = empty.BeginFrameBoundary();
        REQUIRE(noPending.HasError());
        CHECK(HasCode(noPending.ErrorValue(), RenderSurfaceLifecycleErrors::NoPendingRequest));

        RenderSurfaceLifecycle lifecycle = ReadyLifecycle();
        auto invalid = lifecycle.Queue(Command(2, RenderSurfaceCommandKind::Resize));
        REQUIRE(invalid.HasError());
        CHECK(HasCode(invalid.ErrorValue(), RenderSurfaceLifecycleErrors::InvalidCommand));
        invalid = lifecycle.Queue(Command(1, RenderSurfaceCommandKind::Suspend));
        REQUIRE(invalid.HasError());
        CHECK(HasCode(invalid.ErrorValue(), RenderSurfaceLifecycleErrors::StaleRequest));

        QueueResize(lifecycle, 2, 1600, 2);
        const auto transition = lifecycle.BeginFrameBoundary();
        REQUIRE(transition.HasValue());
        const auto busy = lifecycle.BeginFrameBoundary();
        REQUIRE(busy.HasError());
        CHECK(HasCode(busy.ErrorValue(), RenderSurfaceLifecycleErrors::TransitionBusy));
        RenderSurfaceTransition stale = transition.Value();
        ++stale.command.sequence;
        const auto staleResult = lifecycle.Complete(stale, RenderSurfaceRealization::Ready);
        REQUIRE(staleResult.HasError());
        CHECK(HasCode(staleResult.ErrorValue(), RenderSurfaceLifecycleErrors::StaleTransition));
        const auto invalidOutcome = lifecycle.Complete(transition.Value(), RenderSurfaceRealization::Unattached);
        REQUIRE(invalidOutcome.HasError());
        CHECK(HasCode(invalidOutcome.ErrorValue(), RenderSurfaceLifecycleErrors::InvalidOutcome));
        CHECK(lifecycle.Complete(transition.Value(), RenderSurfaceRealization::Ready).HasValue());

        REQUIRE(lifecycle.Queue(Command(3, RenderSurfaceCommandKind::Lose)).HasValue());
        CHECK(Complete(lifecycle, RenderSurfaceRealization::Lost).state == RenderSurfaceState::Lost);
        const auto repeatedLoss = lifecycle.Queue(Command(4, RenderSurfaceCommandKind::Lose));
        REQUIRE(repeatedLoss.HasError());
        CHECK(HasCode(repeatedLoss.ErrorValue(), RenderSurfaceLifecycleErrors::InvalidState));
    }

    TEST_CASE("Surface lifecycle enforces renderer owner-thread affinity", "[runtime][renderer][surface]") {
        RenderSurfaceLifecycle lifecycle = ReadyLifecycle();
        bool queueRejected = false;
        bool snapshotRejected = false;
        bool stateQueryRejected = false;
        std::thread foreign([&] {
            const auto queued = lifecycle.Queue(Command(2, RenderSurfaceCommandKind::Suspend));
            queueRejected = queued.HasError() && HasCode(queued.ErrorValue(), RenderSurfaceLifecycleErrors::WrongThread);
            const auto snapshot = lifecycle.Snapshot();
            snapshotRejected = snapshot.HasError() && HasCode(snapshot.ErrorValue(), RenderSurfaceLifecycleErrors::WrongThread);
            const auto stateQuery = lifecycle.HasPendingRequest();
            stateQueryRejected = stateQuery.HasError() && HasCode(stateQuery.ErrorValue(), RenderSurfaceLifecycleErrors::WrongThread);
        });
        foreign.join();
        CHECK(queueRejected);
        CHECK(snapshotRejected);
        CHECK(stateQueryRejected);
        CHECK_FALSE(lifecycle.HasPendingRequest().Value());
    }

    TEST_CASE("Surface lifecycle move transfers exact pending and in-flight authority", "[runtime][renderer][surface]") {
        RenderSurfaceLifecycle source = ReadyLifecycle();
        QueueResize(source, 2, 1600, 2);
        const auto frozen = source.BeginFrameBoundary();
        REQUIRE(frozen.HasValue());
        REQUIRE(source.Queue(Command(3, RenderSurfaceCommandKind::Close)).HasValue());

        RenderSurfaceLifecycle lifecycle{std::move(source)};
        const auto inertTransition = source.HasTransitionInFlight();
        const auto inertPending = source.HasPendingRequest();
        REQUIRE(inertTransition.HasError());
        REQUIRE(inertPending.HasError());
        CHECK(HasCode(inertTransition.ErrorValue(), RenderSurfaceLifecycleErrors::WrongThread));
        CHECK(HasCode(inertPending.ErrorValue(), RenderSurfaceLifecycleErrors::WrongThread));
        const auto inertSnapshot = source.Snapshot();
        REQUIRE(inertSnapshot.HasError());
        CHECK(HasCode(inertSnapshot.ErrorValue(), RenderSurfaceLifecycleErrors::WrongThread));

        CHECK(lifecycle.HasTransitionInFlight().Value());
        CHECK(lifecycle.HasPendingRequest().Value());
        const auto resized = lifecycle.Complete(frozen.Value(), RenderSurfaceRealization::Ready);
        REQUIRE(resized.HasValue());
        CHECK(resized.Value().active == Configuration(1600, 2));
        CHECK(Complete(lifecycle, RenderSurfaceRealization::Unattached).state == RenderSurfaceState::Unattached);
    }

    TEST_CASE("Initial attachment may publish suspended without allocating a zero-sized output", "[runtime][renderer][surface]") {
        RenderSurfaceLifecycle lifecycle = Lifecycle(7);
        REQUIRE(lifecycle.Queue(Command(1, RenderSurfaceCommandKind::Attach)).HasValue());
        const RenderSurfaceSnapshot suspended = Complete(lifecycle, RenderSurfaceRealization::Suspended);
        CHECK(suspended.surface == RenderSurfaceId{7, 1});
        CHECK(suspended.state == RenderSurfaceState::Suspended);
        CHECK_FALSE(suspended.active.has_value());
    }

}  // namespace Horo::Render::SurfaceLifecycleTests
