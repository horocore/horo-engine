#include "Horo/Runtime/Ui/UiActions.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Id> Id AuthoredId(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return Id::Create(bytes).Value();
        }

        UiOwnershipGeneration Owner(const std::uint64_t value = 73) {
            return UiOwnershipGeneration::Create(value).Value();
        }

        template <typename Revision> Revision RevisionValue(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        UiActionOwnerContext Context(const std::uint64_t ownerValue = 73, const std::uint64_t interaction = 3) {
            const UiOwnershipGeneration owner = Owner(ownerValue);
            return {{owner, 1, 1},
                    {owner, 2, 1},
                    AuthoredId<UiDocumentId>(1),
                    RevisionValue<UiDocumentRevision>(1),
                    RevisionValue<UiRuntimeTreeRevision>(2),
                    RevisionValue<UiInteractionRevision>(interaction)};
        }

        UiActionSource Source(const UiActionOwnerContext &context = Context()) {
            return {context, {context.instance.ownership, 3, 1}};
        }

        UiActionPayload TextPayload(const std::string_view value) {
            auto text = UiActionText::Create(value);
            REQUIRE(text.HasValue());
            const auto payload = UiActionPayload::Create(std::array<UiActionValue, 1>{text.Value()});
            REQUIRE(payload.HasValue());
            return std::move(payload).Value();
        }

        void ExpectError(const Result<void> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        template <typename T> void ExpectError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        class RecordingHandler final : public UiActionHandler {
        public:
            Result<UiActionResult> Handle(const UiActionRequest &request) override {
                ++calls;
                CHECK(request.origin == UiActionOriginOf(request.command));
                if (UiActionCommandKindOf(request.command) == UiActionCommandKind::Navigation) {
                    auto navigation = UiDefaultNavigationResult::FocusMoved(request.source.element,
                                                                            UiElementHandle{request.source.owner.instance.ownership, 4, 1});
                    REQUIRE(navigation.HasValue());
                    return UiActionResult::CompletedNavigation(request.id, std::move(navigation).Value());
                }
                return UiActionResult::Completed(request.id, TextPayload("done"));
            }

            std::size_t calls{};
        };

        class ThrowingHandler final : public UiActionHandler {
        public:
            Result<UiActionResult> Handle(const UiActionRequest &) override {
                throw std::runtime_error("test handler failure");
            }
        };

        class ReentrantHandler final : public UiActionHandler {
        public:
            explicit ReentrantHandler(UiActionRouter &router) : router_(router) {}

            Result<UiActionResult> Handle(const UiActionRequest &request) override {
                ExpectError(router_.TryDequeue(), UiErrors::ActionLifecycleUnavailable);
                return UiActionResult::Handled(request.id);
            }

        private:
            UiActionRouter &router_;
        };

        UiActionRouter Router(const std::uint32_t capacity = 4) {
            auto router = UiActionRouter::Create({Context(), capacity});
            REQUIRE(router.HasValue());
            return std::move(router).Value();
        }

        TEST_CASE("Runtime UI action identities and payloads remain typed and bounded", "[runtime_ui][actions][payload]") {
            static_assert(!std::is_same_v<UiActionId, UiDocumentId>);
            static_assert(!std::is_same_v<UiActionId, UiElementId>);
            static_assert(!std::is_same_v<UiActionId, UiCanvasId>);

            const auto text = UiActionText::Create("resume");
            REQUIRE(text.HasValue());
            CHECK(text.Value().View() == "resume");

            UiActionPayload payload;
            REQUIRE(payload.Add(true).HasValue());
            REQUIRE(payload.Add(std::int64_t{-2}).HasValue());
            REQUIRE(payload.Add(text.Value()).HasValue());
            REQUIRE(payload.Validate().HasValue());
            CHECK(payload.Size() == 3);

            for (std::size_t index = payload.Size(); index < MaximumUiActionArguments; ++index)
                REQUIRE(payload.Add(static_cast<std::uint64_t>(index)).HasValue());
            ExpectError(payload.Add(false), UiErrors::ActionPayloadCapacityExceeded);

            const std::string tooLong(MaximumUiActionTextBytes + 1, 'x');
            REQUIRE(UiActionText::Create(tooLong).HasError());
            REQUIRE(UiActionPayload::Create(std::array<UiActionValue, MaximumUiActionArguments + 1>{}).HasError());
            REQUIRE(payload.Add(std::numeric_limits<double>::infinity()).HasError());
        }

        TEST_CASE("Typed action commands validate stable IDs and default navigation evidence", "[runtime_ui][actions][commands]") {
            const UiActionCommand button = UiButtonActionCommand{AuthoredId<UiActionId>(1), TextPayload("play")};
            const UiActionCommand form = UiFormActionCommand{AuthoredId<UiActionId>(2), UiFormActionKind::Submit, {}};
            const UiActionCommand route = UiRouteActionCommand{AuthoredId<UiActionId>(3), UiRouteActionKind::Push, {}};
            const UiActionCommand gameplay = UiGameplayActionCommand{AuthoredId<UiActionId>(4), {}};
            const UiActionCommand navigation{UiNavigationCommand{UiNavigationDirection::Next, Source().element}};

            REQUIRE(ValidateUiActionCommand(button).HasValue());
            REQUIRE(ValidateUiActionCommand(form).HasValue());
            REQUIRE(ValidateUiActionCommand(route).HasValue());
            REQUIRE(ValidateUiActionCommand(gameplay).HasValue());
            REQUIRE(ValidateUiActionCommand(navigation).HasValue());
            CHECK(UiActionOriginOf(button) == UiActionOrigin::Button);
            CHECK(UiActionOriginOf(form) == UiActionOrigin::Form);
            CHECK(UiActionOriginOf(route) == UiActionOrigin::Route);
            CHECK(UiActionOriginOf(gameplay) == UiActionOrigin::Gameplay);
            CHECK(UiActionOriginOf(navigation) == UiActionOrigin::Navigation);

            auto invalid = UiRouteActionCommand{};
            invalid.kind = UiRouteActionKind::Count;
            ExpectError(ValidateUiActionCommand(invalid), UiErrors::ActionCommandInvalid);

            const UiElementHandle from = Source().element;
            const UiElementHandle target{from.ownership, 4, 1};
            const auto moved = UiDefaultNavigationResult::FocusMoved(from, target);
            REQUIRE(moved.HasValue());
            CHECK(moved.Value().IsValid());
            REQUIRE(UiDefaultNavigationResult::FocusMoved(from, from).HasError());
            REQUIRE(UiDefaultNavigationResult::FocusMoved(from, UiElementHandle{Owner(74), 4, 1}).HasError());
            REQUIRE(UiDefaultNavigationResult::NoTarget({}).HasError());

            const UiActionRequest foreignNavigation{{Owner(), RevisionValue<UiActionSequence>(1)},
                                                    Source(),
                                                    UiActionOrigin::Navigation,
                                                    UiNavigationCommand{UiNavigationDirection::Next, UiElementHandle{Owner(74), 3, 1}}};
            ExpectError(foreignNavigation.Validate(), UiErrors::NavigationInvalid);
        }

        TEST_CASE("Action results distinguish handled, rejected, pending, completed and cancelled states",
                  "[runtime_ui][actions][results]") {
            const UiActionRequestId request{Owner(), RevisionValue<UiActionSequence>(1)};
            const UiActionOperationId operation{Owner(), RevisionValue<UiActionOperationSequence>(4)};

            const auto handled = UiActionResult::Handled(request);
            REQUIRE(handled.HasValue());
            CHECK(handled.Value().kind == UiActionResultKind::Handled);
            CHECK_FALSE(handled.Value().IsTerminal());

            const auto rejected = UiActionResult::Rejected(request, UiActionRejectionReason::NotAuthorized);
            REQUIRE(rejected.HasValue());
            CHECK(rejected.Value().IsTerminal());
            REQUIRE(rejected.Value().Validate().HasValue());

            const auto pending = UiActionResult::Pending(request, operation);
            REQUIRE(pending.HasValue());
            CHECK_FALSE(pending.Value().IsTerminal());

            const auto completed = UiActionResult::Completed(request, TextPayload("ok"), operation);
            REQUIRE(completed.HasValue());
            CHECK(completed.Value().IsTerminal());

            const auto cancelled = UiActionResult::Cancelled(request, operation, UiActionCancellationReason::Reload);
            REQUIRE(cancelled.HasValue());
            CHECK(cancelled.Value().IsTerminal());

            UiActionResult malformed = pending.Value();
            malformed.hasOperation = false;
            ExpectError(malformed.Validate(), UiErrors::ActionResultInvalid);

            const UiActionOperationId foreign{Owner(74), RevisionValue<UiActionOperationSequence>(4)};
            ExpectError(UiActionResult::Pending(request, foreign), UiErrors::ActionResultInvalid);

            const auto navigation = UiDefaultNavigationResult::SubmitDispatched(Source().element);
            REQUIRE(navigation.HasValue());
            const auto navigationResult = UiActionResult::CompletedNavigation(request, navigation.Value());
            REQUIRE(navigationResult.HasValue());
            CHECK(navigationResult.Value().navigation->outcome == UiDefaultNavigationOutcome::SubmitDispatched);

            const auto foreignNavigation = UiDefaultNavigationResult::SubmitDispatched(UiElementHandle{Owner(74), 3, 1});
            REQUIRE(foreignNavigation.HasValue());
            ExpectError(UiActionResult::CompletedNavigation(request, foreignNavigation.Value()), UiErrors::NavigationInvalid);
        }

        TEST_CASE("Action router preallocates queue, fences revisions, and closes admission during reload/shutdown",
                  "[runtime_ui][actions][router]") {
            auto router = Router(1);
            const auto allocationsBefore = ::Horo::Tests::AllocationProbe::Count();
            const auto request = router.Enqueue(Source(), UiButtonActionCommand{AuthoredId<UiActionId>(1), {}});
            const auto allocationsAfter = ::Horo::Tests::AllocationProbe::Count();
            REQUIRE(request.HasValue());
            CHECK(allocationsAfter == allocationsBefore);
            CHECK(router.QueuedCount() == 1);
            ExpectError(router.Enqueue(Source(), UiButtonActionCommand{AuthoredId<UiActionId>(2), {}}),
                        UiErrors::ActionQueueCapacityExceeded);

            RecordingHandler handler;
            const auto dispatched = router.DispatchNext(handler);
            REQUIRE(dispatched.HasValue());
            REQUIRE(dispatched.Value().has_value());
            CHECK(dispatched.Value()->kind == UiActionResultKind::Completed);
            CHECK(handler.calls == 1);
            CHECK(router.QueuedCount() == 0);

            auto staleSource = Source(Context(73, 4));
            ExpectError(router.Enqueue(staleSource, UiButtonActionCommand{AuthoredId<UiActionId>(3), {}}), UiErrors::ActionSourceStale);

            REQUIRE(router.BeginRetirement().HasValue());
            ExpectError(router.Enqueue(Source(), UiButtonActionCommand{AuthoredId<UiActionId>(4), {}}),
                        UiErrors::ActionLifecycleUnavailable);
            router.Shutdown();
            router.Shutdown();
            CHECK(router.State() == UiActionRouterState::Stopped);
            ExpectError(router.TryDequeue(), UiErrors::ActionLifecycleUnavailable);
        }

        TEST_CASE("Navigation dispatch returns a typed completed result without allocating in the hot path",
                  "[runtime_ui][actions][navigation]") {
            auto router = Router();
            REQUIRE(router.Enqueue(Source(), UiNavigationCommand{UiNavigationDirection::Next, Source().element}).HasValue());
            RecordingHandler handler;
            const auto allocationsBefore = ::Horo::Tests::AllocationProbe::Count();
            const auto result = router.DispatchNext(handler);
            const auto allocationsAfter = ::Horo::Tests::AllocationProbe::Count();
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().has_value());
            CHECK(result.Value()->navigation.has_value());
            CHECK(result.Value()->navigation->outcome == UiDefaultNavigationOutcome::FocusMoved);
            CHECK(allocationsAfter == allocationsBefore);
        }

        TEST_CASE("Action router contains handler failures and reentrant queue access", "[runtime_ui][actions][router]") {
            auto router = Router();
            REQUIRE(router.Enqueue(Source(), UiButtonActionCommand{AuthoredId<UiActionId>(5), {}}).HasValue());

            ThrowingHandler throwing;
            ExpectError(router.DispatchNext(throwing), UiErrors::ActionHandlerFailed);

            REQUIRE(router.Enqueue(Source(), UiButtonActionCommand{AuthoredId<UiActionId>(6), {}}).HasValue());
            ReentrantHandler reentrant(router);
            const auto result = router.DispatchNext(reentrant);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().has_value());
            CHECK(result.Value()->kind == UiActionResultKind::Handled);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
