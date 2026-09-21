#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiPointerCapture.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Identity> Identity Stable(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return Identity::Create(bytes).Value();
        }

        UiOwnershipGeneration Owner(const std::uint64_t value = 37) {
            return UiOwnershipGeneration::Create(value).Value();
        }

        template <typename Revision> Revision RevisionValue(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        UiElementTreeDescriptor TreeDescriptor() {
            return {{Owner(), 1, 1},
                    {Owner(), 2, 1},
                    Stable<UiDocumentId>(1),
                    RevisionValue<UiDocumentRevision>(1),
                    RevisionValue<UiRuntimeTreeRevision>(1),
                    {8, 4, 4}};
        }

        UiElementTree Tree() {
            auto slots = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            const std::array elements{UiElementDescriptor{Stable<UiElementId>(1), {}},
                                      UiElementDescriptor{Stable<UiElementId>(2), Stable<UiElementId>(1)},
                                      UiElementDescriptor{Stable<UiElementId>(3), Stable<UiElementId>(2)},
                                      UiElementDescriptor{Stable<UiElementId>(4), Stable<UiElementId>(1)}};
            auto tree = UiElementTree::Create(slots, TreeDescriptor(), elements);
            REQUIRE(tree.HasValue());
            return std::move(tree).Value();
        }

        UiElementHandle Element(const UiElementTree &tree, const std::uint8_t marker) {
            return tree.Find(Stable<UiElementId>(marker)).Value();
        }

        UiRenderViewId View() {
            return {Owner(), 9, 1};
        }

        RuntimeUiInputContextId Context(const std::uint32_t slot = 7) {
            return {Owner(), slot, 1};
        }

        UiPointerId Pointer(const std::uint32_t value) {
            return UiPointerId::Create(value).Value();
        }

        UiPresentedInteractionState Presented(const UiInteractionRevision interaction = RevisionValue<UiInteractionRevision>(9),
                                              const UiRenderSnapshotRevision snapshot = RevisionValue<UiRenderSnapshotRevision>(1)) {
            auto presented = UiPresentedInteractionState::Create(View(), {Owner(), 2, 1}).Value();
            REQUIRE(presented
                        .Apply({presented.View(), presented.Canvas(), interaction, snapshot, UiPresentationOutcome::Presented,
                                UiPresentationReason::None})
                        .HasValue());
            return presented;
        }

        UiEventRoute Route(const UiElementTree &tree, const UiElementHandle target,
                           const UiInteractionRevision interaction = RevisionValue<UiInteractionRevision>(9)) {
            const auto descriptor = TreeDescriptor();
            return {descriptor.instance, descriptor.canvas, descriptor.document, tree.Revision(), interaction, target, std::nullopt};
        }

        UiPointerCaptureRequest Request(const UiElementTree &tree, const UiElementHandle target, const std::uint32_t pointer = 1,
                                        const RuntimeUiInputContextId context = Context(),
                                        const UiInteractionRevision interaction = RevisionValue<UiInteractionRevision>(9)) {
            return {context, Pointer(pointer), UiPointerButton::Primary, View(), Route(tree, target, interaction)};
        }

        UiPointerCaptureStore Store(const std::uint32_t capacity = 4) {
            auto store = UiPointerCaptureStore::Create({Owner(), capacity});
            REQUIRE(store.HasValue());
            return std::move(store).Value();
        }

        void RequireError(const auto &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        TEST_CASE("Runtime UI pointer capture admits exact presented targets without frame-hot allocation",
                  "[runtime_ui][pointer_capture][normal]") {
            auto tree = Tree();
            auto presented = Presented();
            auto store = Store();
            const auto request = Request(tree, Element(tree, 3));

            const auto before = ::Horo::Tests::AllocationProbe::Count();
            auto captured = store.Capture(request, tree, presented);
            const auto after = ::Horo::Tests::AllocationProbe::Count();
            REQUIRE(captured.HasValue());
            CHECK(after == before);

            auto token = std::move(captured).Value();
            CHECK(token.IsActive());
            CHECK(token.State() == UiPointerCaptureState::Active);
            CHECK(token.Request().context == request.context);
            CHECK(token.Request().pointer == request.pointer);
            CHECK(token.Request().route.target == request.route.target);
            CHECK(store.ActiveCount() == 1);

            token.Release();
            CHECK_FALSE(token.IsActive());
            CHECK(token.State() == UiPointerCaptureState::Inactive);
            CHECK(store.ActiveCount() == 0);
            CHECK(store.IsDrained());
        }

        TEST_CASE("Runtime UI pointer capture is per context and pointer, and stale leases cannot release reused slots",
                  "[runtime_ui][pointer_capture][edge]") {
            auto tree = Tree();
            auto presented = Presented();
            auto store = Store(2);
            const auto target = Element(tree, 3);

            auto first = std::move(store.Capture(Request(tree, target, 1), tree, presented)).Value();
            auto busyRequest = Request(tree, target, 1);
            busyRequest.button = UiPointerButton::Secondary;
            RequireError(store.Capture(busyRequest, tree, presented), UiErrors::PointerCaptureBusy);

            auto second = std::move(store.Capture(Request(tree, target, 2), tree, presented)).Value();
            RequireError(store.Capture(Request(tree, target, 3), tree, presented), UiErrors::PointerCaptureCapacityExceeded);

            REQUIRE(store.ReleasePointer(first.Request().context, first.Request().pointer, first.Request().button).HasValue());
            CHECK_FALSE(first.IsActive());
            auto reused = std::move(store.Capture(Request(tree, target, 1), tree, presented)).Value();
            first.Release();
            CHECK(reused.IsActive());
            second.Release();
            reused.Release();
            CHECK(store.IsDrained());

            auto otherContext = std::move(store.Capture(Request(tree, target, 1, Context(8)), tree, presented)).Value();
            otherContext.Release();
        }

        TEST_CASE("Runtime UI pointer capture rejects malformed, foreign, stale, and unpresented evidence",
                  "[runtime_ui][pointer_capture][validation]") {
            auto tree = Tree();
            auto presented = Presented();
            auto store = Store();
            const auto target = Element(tree, 3);

            UiPointerCaptureRequest malformed{};
            RequireError(store.Capture(malformed, tree, presented), UiErrors::PointerCaptureInvalid);

            auto foreignView = Request(tree, target);
            ++foreignView.view.generation;
            RequireError(store.Capture(foreignView, tree, presented), UiErrors::PointerCaptureSourceStale);

            auto staleTarget = Request(tree, target);
            ++staleTarget.route.target.generation;
            RequireError(store.Capture(staleTarget, tree, presented), UiErrors::PointerCaptureSourceStale);

            auto unpresented = Request(tree, target, 2, Context(), RevisionValue<UiInteractionRevision>(10));
            RequireError(store.Capture(unpresented, tree, presented), UiErrors::PointerCaptureInteractionStale);

            CHECK(UiPointerId::Create(0).HasError());
            CHECK(UiPointerCaptureStoreDescriptor{Owner(), 0}.IsValid() == false);
            CHECK(UiPointerCaptureStore::Create({Owner(), MaximumUiPointerCaptures + 1}).HasError());
        }

        TEST_CASE("Runtime UI pointer capture cancellation is synchronous, typed, and exactly once per lease",
                  "[runtime_ui][pointer_capture][cancellation]") {
            auto tree = Tree();
            auto presented = Presented();
            auto store = Store();
            auto token = std::move(store.Capture(Request(tree, Element(tree, 3)), tree, presented)).Value();

            auto cancelled = store.CancelContext(Context(), UiPointerCaptureCancellationReason::FocusLost);
            REQUIRE(cancelled.HasValue());
            CHECK(cancelled.Value() == 1);
            CHECK_FALSE(token.IsActive());
            CHECK(token.State() == UiPointerCaptureState::Cancelled);
            REQUIRE(token.CancellationReason().has_value());
            CHECK(*token.CancellationReason() == UiPointerCaptureCancellationReason::FocusLost);
            CHECK(store.ActiveCount() == 0);
            CHECK_FALSE(store.IsDrained());

            REQUIRE(store.CancelContext(Context(), UiPointerCaptureCancellationReason::FocusLost).HasValue());
            CHECK(store.CancelContext(Context(), UiPointerCaptureCancellationReason::FocusLost).Value() == 0);
            token.Release();
            CHECK(store.IsDrained());

            auto cancellable = std::move(store.Capture(Request(tree, Element(tree, 3), 3), tree, presented)).Value();
            REQUIRE(store.CancelContext(Context(), UiPointerCaptureCancellationReason::ModalOpened).HasValue());
            auto recaptured = std::move(store.Capture(Request(tree, Element(tree, 3), 3), tree, presented)).Value();
            CHECK(recaptured.IsActive());
            cancellable.Release();
            recaptured.Release();

            auto second = std::move(store.Capture(Request(tree, Element(tree, 3), 2), tree, presented)).Value();
            REQUIRE(store.CancelInstance(TreeDescriptor().instance, UiPointerCaptureCancellationReason::ScopeDestroyed).HasValue());
            CHECK(second.CancellationReason() == UiPointerCaptureCancellationReason::ScopeDestroyed);
            second.Release();

            auto viewportCapture = std::move(store.Capture(Request(tree, Element(tree, 3), 4), tree, presented)).Value();
            REQUIRE(store.CancelView(View(), UiPointerCaptureCancellationReason::ViewportDestroyed).HasValue());
            CHECK(viewportCapture.CancellationReason() == UiPointerCaptureCancellationReason::ViewportDestroyed);
            viewportCapture.Release();
        }

        TEST_CASE("Runtime UI pointer capture reconciles reload generations and shuts down without dangling leases",
                  "[runtime_ui][pointer_capture][lifetime]") {
            auto tree = Tree();
            auto presented = Presented();
            auto store = Store();
            auto token = std::move(store.Capture(Request(tree, Element(tree, 3)), tree, presented)).Value();

            const auto modal = Element(tree, 2);
            auto mutation = std::move(UiStructuralCommandBuffer::Create(tree.Instance(), tree.Canvas(), tree.SourceDocumentRevision(),
                                                                        tree.Revision(), 1))
                                .Value();
            REQUIRE(mutation.Add(UiReparentElementCommand{Element(tree, 4), modal, 1}).HasValue());
            REQUIRE(tree.CommitDeferred(mutation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());

            auto nextPresented = Presented(RevisionValue<UiInteractionRevision>(10), RevisionValue<UiRenderSnapshotRevision>(2));
            const UiPointerCaptureContext current{Context(),
                                                  nextPresented.View(),
                                                  tree.Instance(),
                                                  tree.Canvas(),
                                                  tree.SourceDocument(),
                                                  tree.Revision(),
                                                  nextPresented.LastPresentedInteraction()};
            const auto reconciled = store.Reconcile(current, tree, nextPresented);
            REQUIRE(reconciled.HasValue());
            CHECK(reconciled.Value() == 1);
            CHECK(token.CancellationReason() == UiPointerCaptureCancellationReason::Reload);
            token.Release();

            UiPointerCaptureToken surviving;
            {
                auto shutdownStore = Store();
                surviving =
                    std::move(shutdownStore.Capture(Request(tree, Element(tree, 3), 2, Context(), RevisionValue<UiInteractionRevision>(10)),
                                                    tree, nextPresented))
                        .Value();
                shutdownStore.Shutdown();
                shutdownStore.Shutdown();
                CHECK(shutdownStore.State() == UiPointerCaptureStoreState::Stopped);
                CHECK_FALSE(shutdownStore.IsDrained());
                CHECK(surviving.CancellationReason() == UiPointerCaptureCancellationReason::Shutdown);
                RequireError(shutdownStore.Capture(Request(tree, Element(tree, 3), 3), tree, nextPresented),
                             UiErrors::PointerCaptureLifecycleUnavailable);
            }
            CHECK(surviving.State() == UiPointerCaptureState::Cancelled);
            surviving.Release();
            CHECK_FALSE(surviving.IsActive());
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
