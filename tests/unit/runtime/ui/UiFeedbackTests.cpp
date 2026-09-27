#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiFeedback.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Id> Id Stable(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return Id::Create(bytes).Value();
        }

        template <typename Revision> Revision RevisionValue(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        UiActionOwnerContext Owner(const std::uint64_t generation = 31, const std::uint64_t interaction = 3) {
            const auto ownership = UiOwnershipGeneration::Create(generation).Value();
            return {{ownership, 1, 1},
                    {ownership, 2, 1},
                    Stable<UiDocumentId>(1),
                    RevisionValue<UiDocumentRevision>(1),
                    RevisionValue<UiRuntimeTreeRevision>(2),
                    RevisionValue<UiInteractionRevision>(interaction)};
        }

        UiActionSource Source(const UiActionOwnerContext &owner = Owner()) {
            return {owner, {owner.instance.ownership, 3, 1}};
        }

        UiActionRequest NavigationRequest(const UiActionSource &source = Source()) {
            return {{source.owner.instance.ownership, RevisionValue<UiActionSequence>(1)},
                    source,
                    UiActionOrigin::Navigation,
                    UiNavigationCommand{UiNavigationDirection::Next, source.element}};
        }

        UiFeedbackQueue Queue(const UiActionOwnerContext &owner = Owner(), const std::uint32_t capacity = 8) {
            auto created = UiFeedbackQueue::Create({owner, capacity});
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        template <typename T> void ExpectError(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        UiActionResult NavigationResult(const UiActionRequest &request, const UiDefaultNavigationResult &navigation) {
            auto result = UiActionResult::CompletedNavigation(request.id, navigation);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        UiFeedbackKind PopKind(UiFeedbackQueue &queue) {
            auto popped = queue.TryDequeue();
            REQUIRE(popped.HasValue());
            REQUIRE(popped.Value().has_value());
            return popped.Value()->kind;
        }

        TEST_CASE("Runtime UI feedback emits typed navigation, confirmation, cancellation and boundary intents", "[runtime_ui][feedback]") {
            auto queue = Queue();
            const auto request = NavigationRequest();
            const UiElementHandle target{request.source.owner.instance.ownership, 4, 1};
            const auto moved = UiDefaultNavigationResult::FocusMoved(request.source.element, target);
            REQUIRE(moved.HasValue());
            REQUIRE(queue.ObserveAction(request, NavigationResult(request, moved.Value())).Value());
            CHECK(PopKind(queue) == UiFeedbackKind::Navigate);

            const auto submitted = UiDefaultNavigationResult::SubmitDispatched(request.source.element);
            REQUIRE(submitted.HasValue());
            REQUIRE(queue.ObserveAction(request, NavigationResult(request, submitted.Value())).Value());
            CHECK(PopKind(queue) == UiFeedbackKind::Confirm);

            const auto cancelled = UiDefaultNavigationResult::CancelDispatched(request.source.element);
            REQUIRE(cancelled.HasValue());
            REQUIRE(queue.ObserveAction(request, NavigationResult(request, cancelled.Value())).Value());
            CHECK(PopKind(queue) == UiFeedbackKind::Cancel);

            const auto boundary = UiDefaultNavigationResult::NoTarget(request.source.element);
            REQUIRE(boundary.HasValue());
            REQUIRE(queue.ObserveAction(request, NavigationResult(request, boundary.Value())).Value());
            CHECK(PopKind(queue) == UiFeedbackKind::Boundary);
        }

        TEST_CASE("Runtime UI feedback consumes a real owner-admitted navigation outcome", "[runtime_ui][feedback]") {
            auto routerResult = UiActionRouter::Create({Owner(), 2});
            REQUIRE(routerResult.HasValue());
            auto router = std::move(routerResult).Value();
            const auto source = Source();
            const auto admitted = router.Enqueue(source, UiNavigationCommand{UiNavigationDirection::Next, source.element});
            REQUIRE(admitted.HasValue());
            const auto dequeued = router.TryDequeue();
            REQUIRE(dequeued.HasValue());
            REQUIRE(dequeued.Value().has_value());
            const UiElementHandle target{source.owner.instance.ownership, 4, 1};
            const auto navigation = UiDefaultNavigationResult::FocusMoved(source.element, target);
            REQUIRE(navigation.HasValue());
            auto queue = Queue();
            REQUIRE(queue.ObserveAction(*dequeued.Value(), NavigationResult(*dequeued.Value(), navigation.Value())).Value());
            CHECK(PopKind(queue) == UiFeedbackKind::Navigate);
        }

        TEST_CASE("Runtime UI feedback observes focus audience and explicit action errors", "[runtime_ui][feedback]") {
            auto queue = Queue();
            const auto owner = Owner();
            const UiFocusScope scope{UiFocusPlayerId{owner.instance.ownership, 5, 1},
                                     UiFocusPresentationLayerId{owner.instance.ownership, 6, 1}};
            const UiFocusOwnerContext focusOwner{owner.instance,     owner.canvas,      owner.document, owner.documentRevision,
                                                 owner.treeRevision, owner.interaction, scope};
            const UiFocusTarget target{Stable<UiElementId>(2), Source().element};
            const UiFocusChange focused{UiFocusChangeKind::FocusMoved, UiFocusChangeReason::Explicit, std::nullopt, target, std::nullopt};
            REQUIRE(queue.ObserveFocus(focusOwner, focused).Value());
            auto intent = queue.TryDequeue();
            REQUIRE(intent.HasValue());
            REQUIRE(intent.Value().has_value());
            CHECK(intent.Value()->kind == UiFeedbackKind::Focus);
            CHECK(intent.Value()->focusScope == scope);

            const auto request = NavigationRequest();
            const auto rejected = UiActionResult::Rejected(request.id, UiActionRejectionReason::Unavailable);
            REQUIRE(rejected.HasValue());
            REQUIRE(queue.ObserveAction(request, rejected.Value()).Value());
            CHECK(PopKind(queue) == UiFeedbackKind::Error);
            const UiFocusChange noTarget{UiFocusChangeKind::NoTarget, UiFocusChangeReason::InvalidTarget, target, target, std::nullopt};
            REQUIRE(queue.ObserveFocus(focusOwner, noTarget).Value());
            CHECK(PopKind(queue) == UiFeedbackKind::Boundary);
            const UiFocusChange emptyFocus{UiFocusChangeKind::NoTarget, UiFocusChangeReason::InvalidTarget};
            CHECK_FALSE(queue.ObserveFocus(focusOwner, emptyFocus).Value());
        }

        TEST_CASE("Runtime UI feedback only observes applied control defaults and genuine cancellation", "[runtime_ui][feedback]") {
            auto queue = Queue();
            const UiControlDefaultAction
                applied{Source(), Stable<UiActionId>(9), UiControlActionKind::Activate, UiControlActivationSource::Keyboard, 8, false, {}};
            REQUIRE(queue.ObserveControl(applied).Value());
            CHECK(PopKind(queue) == UiFeedbackKind::Confirm);
            const UiControlInput input{Source(), UiControlInputKind::Cancel, UiControlActivationSource::Keyboard, 9};
            CHECK_FALSE(queue.ObserveControlTransition(input, UiControlTransitionKind::NoOp).Value());
            REQUIRE(queue.ObserveControlTransition(input, UiControlTransitionKind::Cancelled).Value());
            CHECK(PopKind(queue) == UiFeedbackKind::Cancel);
            CHECK(queue.QueuedCount() == 0);
        }

        TEST_CASE("Runtime UI feedback rejects stale generations, capacity and invalid outcome correlation", "[runtime_ui][feedback]") {
            auto queue = Queue(Owner(), 1);
            const auto request = NavigationRequest();
            const auto rejected = UiActionResult::Rejected(request.id, UiActionRejectionReason::Busy);
            REQUIRE(rejected.HasValue());
            REQUIRE(queue.ObserveAction(request, rejected.Value()).Value());
            ExpectError(queue.ObserveAction(request, rejected.Value()), UiErrors::FeedbackCapacityExceeded);
            const auto staleRequest = NavigationRequest(Source(Owner(32)));
            const auto staleResult = UiActionResult::Rejected(staleRequest.id, UiActionRejectionReason::Busy);
            REQUIRE(staleResult.HasValue());
            ExpectError(queue.ObserveAction(staleRequest, staleResult.Value()), UiErrors::FeedbackSourceStale);
            CHECK(PopKind(queue) == UiFeedbackKind::Error);
            const auto wrongResult = UiActionResult::Rejected(staleRequest.id, UiActionRejectionReason::Busy);
            REQUIRE(wrongResult.HasValue());
            ExpectError(queue.ObserveAction(request, wrongResult.Value()), UiErrors::FeedbackInvalid);
        }

        TEST_CASE("Runtime UI feedback preserves FIFO across ring wrap and rejects malformed capacity", "[runtime_ui][feedback]") {
            ExpectError(UiFeedbackQueue::Create({Owner(), 0}), UiErrors::FeedbackInvalid);
            ExpectError(UiFeedbackQueue::Create({Owner(), MaximumUiFeedbackIntents + 1}), UiErrors::FeedbackInvalid);
            auto queue = Queue(Owner(), 2);
            const auto request = NavigationRequest();
            const auto rejected = UiActionResult::Rejected(request.id, UiActionRejectionReason::Busy);
            const auto noTarget = UiActionResult::Rejected(request.id, UiActionRejectionReason::NoTarget);
            REQUIRE(rejected.HasValue());
            REQUIRE(noTarget.HasValue());
            REQUIRE(queue.ObserveAction(request, rejected.Value()).Value());
            CHECK(PopKind(queue) == UiFeedbackKind::Error);
            REQUIRE(queue.ObserveAction(request, noTarget.Value()).Value());
            REQUIRE(queue.ObserveAction(request, rejected.Value()).Value());
            CHECK(PopKind(queue) == UiFeedbackKind::Boundary);
            CHECK(PopKind(queue) == UiFeedbackKind::Error);
        }

        TEST_CASE("Runtime UI feedback leaves stale and retiring outcomes silent", "[runtime_ui][feedback]") {
            auto queue = Queue();
            const auto request = NavigationRequest();
            const auto stale = UiActionResult::Rejected(request.id, UiActionRejectionReason::Stale);
            const auto retiring = UiActionResult::Rejected(request.id, UiActionRejectionReason::Retiring);
            REQUIRE(stale.HasValue());
            REQUIRE(retiring.HasValue());
            CHECK_FALSE(queue.ObserveAction(request, stale.Value()).Value());
            CHECK_FALSE(queue.ObserveAction(request, retiring.Value()).Value());
            CHECK(queue.QueuedCount() == 0);
        }

        TEST_CASE("Runtime UI feedback fences navigation result handles after action admission", "[runtime_ui][feedback]") {
            auto queue = Queue();
            const auto request = NavigationRequest();
            const UiElementHandle differentSource{request.source.owner.instance.ownership, 7, 1};
            const UiElementHandle target{request.source.owner.instance.ownership, 8, 1};
            const auto wrongFrom = UiDefaultNavigationResult::FocusMoved(differentSource, target);
            REQUIRE(wrongFrom.HasValue());
            ExpectError(queue.ObserveAction(request, NavigationResult(request, wrongFrom.Value())), UiErrors::FeedbackSourceStale);

            const auto wrongDispatch = UiDefaultNavigationResult::SubmitDispatched(target);
            REQUIRE(wrongDispatch.HasValue());
            ExpectError(queue.ObserveAction(request, NavigationResult(request, wrongDispatch.Value())), UiErrors::FeedbackSourceStale);
            CHECK(queue.QueuedCount() == 0);
        }

        TEST_CASE("Runtime UI feedback emits only user-requested action cancellation", "[runtime_ui][feedback]") {
            auto queue = Queue();
            const auto request = NavigationRequest();
            const UiActionOperationId operation{request.source.owner.instance.ownership, RevisionValue<UiActionOperationSequence>(4)};
            const auto reload = UiActionResult::Cancelled(request.id, operation, UiActionCancellationReason::Reload);
            const auto requested = UiActionResult::Cancelled(request.id, operation, UiActionCancellationReason::Requested);
            REQUIRE(reload.HasValue());
            REQUIRE(requested.HasValue());
            CHECK_FALSE(queue.ObserveAction(request, reload.Value()).Value());
            REQUIRE(queue.ObserveAction(request, requested.Value()).Value());
            CHECK(PopKind(queue) == UiFeedbackKind::Cancel);
        }

        TEST_CASE("Runtime UI feedback fences a new presented revision and does not cue reload focus recovery", "[runtime_ui][feedback]") {
            auto queue = Queue();
            const auto revisedRequest = NavigationRequest(Source(Owner(31, 4)));
            const auto revisedResult = UiActionResult::Rejected(revisedRequest.id, UiActionRejectionReason::Busy);
            REQUIRE(revisedResult.HasValue());
            ExpectError(queue.ObserveAction(revisedRequest, revisedResult.Value()), UiErrors::FeedbackSourceStale);

            const auto owner = Owner();
            const UiFocusScope scope{UiFocusPlayerId{owner.instance.ownership, 5, 1},
                                     UiFocusPresentationLayerId{owner.instance.ownership, 6, 1}};
            const UiFocusOwnerContext focusOwner{owner.instance,     owner.canvas,      owner.document, owner.documentRevision,
                                                 owner.treeRevision, owner.interaction, scope};
            const UiFocusTarget target{Stable<UiElementId>(2), Source().element};
            const UiFocusChange recovered{UiFocusChangeKind::FocusRecovered, UiFocusChangeReason::Reload, std::nullopt, target,
                                          std::nullopt};
            CHECK_FALSE(queue.ObserveFocus(focusOwner, recovered).Value());
            CHECK(queue.QueuedCount() == 0);
        }

        TEST_CASE("Runtime UI feedback is allocation-free after creation and closes on reload or shutdown", "[runtime_ui][feedback]") {
            auto oldQueue = Queue();
            const auto request = NavigationRequest();
            const auto rejected = UiActionResult::Rejected(request.id, UiActionRejectionReason::Busy);
            REQUIRE(rejected.HasValue());
            const auto before = ::Horo::Tests::AllocationProbe::Count();
            REQUIRE(oldQueue.ObserveAction(request, rejected.Value()).Value());
            const auto intent = oldQueue.TryDequeue();
            REQUIRE(intent.HasValue());
            CHECK(::Horo::Tests::AllocationProbe::Count() == before);

            REQUIRE(oldQueue.ObserveAction(request, rejected.Value()).Value());
            REQUIRE(oldQueue.BeginRetirement().HasValue());
            CHECK(oldQueue.QueuedCount() == 0);
            CHECK_FALSE(oldQueue.TryDequeue().Value().has_value());
            ExpectError(oldQueue.ObserveAction(request, rejected.Value()), UiErrors::FeedbackLifecycleUnavailable);
            auto newQueue = Queue(Owner(32));
            const auto current = NavigationRequest(Source(Owner(32)));
            const auto currentRejected = UiActionResult::Rejected(current.id, UiActionRejectionReason::Busy);
            REQUIRE(currentRejected.HasValue());
            ExpectError(newQueue.ObserveAction(request, rejected.Value()), UiErrors::FeedbackSourceStale);
            REQUIRE(newQueue.ObserveAction(current, currentRejected.Value()).Value());
            oldQueue.Shutdown();
            newQueue.Shutdown();
            CHECK(newQueue.QueuedCount() == 0);
            ExpectError(newQueue.TryDequeue(), UiErrors::FeedbackLifecycleUnavailable);
            oldQueue.Shutdown();
        }

        class RecordingRealizer final : public UiFeedbackRealizer {
        public:
            Result<UiFeedbackRealization> Realize(const UiFeedbackIntent &intent) override {
                observed = intent.kind;
                return Result<UiFeedbackRealization>::Success({UiFeedbackModalityResult::Unavailable, UiFeedbackModalityResult::Skipped});
            }

            UiFeedbackKind observed{UiFeedbackKind::Count};
        };

        class ThrowingRealizer final : public UiFeedbackRealizer {
        public:
            Result<UiFeedbackRealization> Realize(const UiFeedbackIntent &) override {
                throw std::runtime_error("provider failure");
            }
        };

        TEST_CASE("Runtime UI feedback realization is optional, borrowed and failure-isolated", "[runtime_ui][feedback]") {
            auto queue = Queue();
            const auto request = NavigationRequest();
            const auto rejected = UiActionResult::Rejected(request.id, UiActionRejectionReason::Busy);
            REQUIRE(rejected.HasValue());
            REQUIRE(queue.ObserveAction(request, rejected.Value()).Value());
            RecordingRealizer realizer;
            const auto outcome = queue.DeliverNext(realizer);
            REQUIRE(outcome.HasValue());
            REQUIRE(outcome.Value().has_value());
            CHECK(outcome.Value()->audio == UiFeedbackModalityResult::Unavailable);
            CHECK(realizer.observed == UiFeedbackKind::Error);
            CHECK(queue.QueuedCount() == 0);
            REQUIRE(queue.ObserveAction(request, rejected.Value()).Value());
            ThrowingRealizer throwing;
            ExpectError(queue.DeliverNext(throwing), UiErrors::FeedbackConsumerFailed);
            CHECK(queue.QueuedCount() == 0);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
