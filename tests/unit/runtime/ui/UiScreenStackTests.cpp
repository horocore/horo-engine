#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiScreenStack.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
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

        UiRouteStackId Stack(const UiOwnershipGeneration owner = Owner()) {
            return {owner, 1, 1};
        }

        UiRouteMetadata Route(const std::uint8_t marker, const UiPresentationBand band = UiPresentationBand::Screen) {
            return {AuthoredId<UiRouteId>(marker), band, marker, marker == 3};
        }

        UiScreenStack MakeStack(const std::uint32_t capacity = 4) {
            const std::array definitions{Route(1), Route(2), Route(3, UiPresentationBand::Modal)};
            auto stack = UiScreenStack::Create({Owner(), Stack(), definitions, capacity});
            REQUIRE(stack.HasValue());
            return std::move(stack).Value();
        }

        template <typename T> void ExpectError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        void RequireCommitted(const Result<UiRouteOperationResult> &result, const UiRouteOperationKind kind) {
            REQUIRE(result.HasValue());
            CHECK(result.Value().kind == kind);
            CHECK(result.Value().outcome == UiRouteOperationOutcome::Committed);
            CHECK(result.Value().IsTerminal());
            REQUIRE(result.Value().Validate().HasValue());
        }

        TEST_CASE("Route stack descriptors and guards remain owner-scoped and typed", "[runtime_ui][screen_stack][identity]") {
            const auto owner = Owner();
            const std::array definitions{Route(1)};
            auto stack = UiScreenStack::Create({owner, Stack(owner), definitions, 1});
            REQUIRE(stack.HasValue());
            auto screenStack = std::move(stack).Value();

            const auto emptyGuard = screenStack.Guard();
            REQUIRE(emptyGuard.HasValue());
            CHECK(emptyGuard.Value().IsValid());
            CHECK_FALSE(emptyGuard.Value().top.has_value());

            const auto malformed = UiRouteStackGuard::Create({}, UiRouteStackRevision::Create(1).Value());
            ExpectError(malformed, UiErrors::RouteOperationInvalid);

            const std::array duplicateDefinitions{Route(1), Route(1)};
            ExpectError(UiScreenStack::Create({owner, Stack(owner), duplicateDefinitions, 2}), UiErrors::RouteStackInvalid);
            ExpectError(UiScreenStack::Create({owner, Stack(Owner(74)), definitions, 1}), UiErrors::RouteStackInvalid);
        }

        TEST_CASE("Push, pop, replace, back, clear, and guarded navigation commit atomically", "[runtime_ui][screen_stack][operations]") {
            auto stack = MakeStack();
            const auto first = stack.Push(AuthoredId<UiRouteId>(1));
            RequireCommitted(first, UiRouteOperationKind::Push);
            REQUIRE(stack.Top().has_value());
            const auto firstInstance = stack.Top()->id;
            CHECK(stack.Top()->metadata.id == AuthoredId<UiRouteId>(1));
            CHECK(stack.Top()->visibility == UiRouteVisibilityState::Visible);

            const auto second = stack.Push(AuthoredId<UiRouteId>(2));
            RequireCommitted(second, UiRouteOperationKind::Push);
            REQUIRE(stack.Size() == 2);
            CHECK(stack.Routes()[0].visibility == UiRouteVisibilityState::Covered);
            CHECK(stack.Routes()[1].visibility == UiRouteVisibilityState::Visible);

            const auto guarded = stack.Guard();
            REQUIRE(guarded.HasValue());
            const auto backed = stack.Back(guarded.Value());
            RequireCommitted(backed, UiRouteOperationKind::Back);
            REQUIRE(stack.Top().has_value());
            CHECK(stack.Top()->id == firstInstance);

            const auto replaced = stack.Replace(AuthoredId<UiRouteId>(3));
            RequireCommitted(replaced, UiRouteOperationKind::Replace);
            REQUIRE(stack.Top().has_value());
            CHECK(stack.Top()->metadata.id == AuthoredId<UiRouteId>(3));
            CHECK(stack.Top()->id != firstInstance);
            CHECK(stack.Top()->visibility == UiRouteVisibilityState::Visible);

            const auto popped = stack.Pop();
            RequireCommitted(popped, UiRouteOperationKind::Pop);
            CHECK(stack.Empty());

            const auto clearedEmpty = stack.Clear();
            RequireCommitted(clearedEmpty, UiRouteOperationKind::Clear);
            CHECK(stack.Empty());

            REQUIRE(stack.Push(AuthoredId<UiRouteId>(1)).HasValue());
            const auto cleared = stack.Clear();
            RequireCommitted(cleared, UiRouteOperationKind::Clear);
            CHECK(stack.Empty());
        }

        TEST_CASE("Stale guards reject without mutating the last-good stack", "[runtime_ui][screen_stack][stale]") {
            auto stack = MakeStack();
            REQUIRE(stack.Push(AuthoredId<UiRouteId>(1)).HasValue());
            const auto guard = stack.Guard();
            REQUIRE(guard.HasValue());
            const auto accepted = stack.Push(AuthoredId<UiRouteId>(2));
            RequireCommitted(accepted, UiRouteOperationKind::Push);

            const auto stale = stack.Push(AuthoredId<UiRouteId>(3), guard.Value());
            REQUIRE(stale.HasValue());
            CHECK(stale.Value().outcome == UiRouteOperationOutcome::Rejected);
            CHECK(stale.Value().rejection == UiRouteOperationRejection::GuardMismatch);
            CHECK(stale.Value().IsTerminal());
            REQUIRE(stale.Value().Validate().HasValue());
            CHECK(stack.Size() == 2);
            CHECK(stack.Top()->metadata.id == AuthoredId<UiRouteId>(2));
            CHECK(stack.Revision() == accepted.Value().revision);
        }

        TEST_CASE("Prepared transactions preserve state on cancel and reject reentrancy", "[runtime_ui][screen_stack][transaction]") {
            auto stack = MakeStack(2);
            auto prepared = stack.Prepare(UiRouteOperationRequest::Push(AuthoredId<UiRouteId>(1)));
            REQUIRE(prepared.HasValue());
            auto transaction = std::move(prepared).Value();
            CHECK(stack.Empty());
            ExpectError(stack.Push(AuthoredId<UiRouteId>(2)), UiErrors::RouteOperationReentrant);

            const auto cancelled = transaction.Cancel();
            REQUIRE(cancelled.HasValue());
            CHECK(cancelled.Value().outcome == UiRouteOperationOutcome::Rejected);
            CHECK(cancelled.Value().rejection == UiRouteOperationRejection::Cancelled);
            CHECK(stack.Empty());
            ExpectError(transaction.Commit(), UiErrors::RouteOperationAlreadyCompleted);

            auto committed = stack.Prepare(UiRouteOperationRequest::Push(AuthoredId<UiRouteId>(1)));
            REQUIRE(committed.HasValue());
            const auto result = std::move(committed).Value().Commit();
            RequireCommitted(result, UiRouteOperationKind::Push);
            CHECK(stack.Size() == 1);
        }

        TEST_CASE("Route stack steady-state operations do not allocate and lifecycle closes admission",
                  "[runtime_ui][screen_stack][lifecycle]") {
            auto stack = MakeStack(2);
            const auto before = ::Horo::Tests::AllocationProbe::Count();
            const auto pushed = stack.Push(AuthoredId<UiRouteId>(1));
            const auto after = ::Horo::Tests::AllocationProbe::Count();
            RequireCommitted(pushed, UiRouteOperationKind::Push);
            CHECK(after == before);

            REQUIRE(stack.BeginRetirement().HasValue());
            ExpectError(stack.Push(AuthoredId<UiRouteId>(2)), UiErrors::RouteOperationLifecycleUnavailable);
            stack.Shutdown();
            stack.Shutdown();
            CHECK(stack.State() == UiScreenStackState::Stopped);
            CHECK(stack.Empty());
            ExpectError(stack.Guard(), UiErrors::RouteOperationLifecycleUnavailable);
        }

        TEST_CASE("Unknown routes and capacity are terminal rejections", "[runtime_ui][screen_stack][failure]") {
            auto stack = MakeStack(1);
            const auto unknown = stack.Push(AuthoredId<UiRouteId>(99));
            REQUIRE(unknown.HasValue());
            CHECK(unknown.Value().rejection == UiRouteOperationRejection::NotFound);
            CHECK(stack.Empty());

            REQUIRE(stack.Push(AuthoredId<UiRouteId>(1)).HasValue());
            const auto full = stack.Push(AuthoredId<UiRouteId>(2));
            REQUIRE(full.HasValue());
            CHECK(full.Value().rejection == UiRouteOperationRejection::Capacity);
            CHECK(stack.Size() == 1);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
