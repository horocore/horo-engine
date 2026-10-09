#include "Horo/Runtime/Ui/UiOverlayLifecycle.h"
#include "UiHotReloadTestFixture.h"
#include "support/AllocationProbe.h"

#include <array>
#include <limits>

namespace Horo::Runtime::Ui::ReloadTests {
    namespace {
        /** @brief Independent actual runtime namespaces; authored IDs and viewport may legitimately be shared. */
        LayerOptions Options(std::uint32_t index, UiPresentationBand band = UiPresentationBand::Screen) {
            LayerOptions value;
            value.route = {Stable<UiRouteId>(9), band, index, band == UiPresentationBand::Modal};
            value.instance = {Owner(), 100 + index, 1};
            value.canvas = {Owner(), 200 + index, 1};
            value.layer = {Owner(), 300 + index, 1};
            return value;
        }

        UiOverlayLayerDescriptor Binding(const LayerOptions &value, UiModalExclusivity exclusivity = UiModalExclusivity::None,
                                         UiOverlayViewportId viewport = {Owner(), 500, 1}) {
            UiOverlayLayerDescriptor binding{Stable<UiCanvasId>(2),
                                             value.route.id,
                                             {Owner(), 400 + value.instance.slot, 1},
                                             value.view,
                                             viewport,
                                             value.player,
                                             exclusivity};
            if (exclusivity != UiModalExclusivity::None) {
                binding.modalRoot = Stable<UiElementId>(10);
                binding.defaultFocus = Stable<UiElementId>(11);
            }
            return binding;
        }

        /** @brief Real Assets-loaded publisher with its actual authored route active before transfer. */
        UiHotReload Publisher(UiElementSlotAllocator &allocator, const LayerOptions &options) {
            auto created = UiHotReload::Create(Generation(allocator, 1, 32, false, 0, 0, 3, options));
            REQUIRE(created.HasValue());
            auto publisher = std::move(created).Value();
            REQUIRE(Canvas(publisher).routes->Push(options.route.id).Value().IsCommitted());
            return publisher;
        }

        struct OverlayRig final {
            UiElementSlotAllocator allocator{std::move(UiElementSlotAllocator::Create(Owner())).Value()};
            UiOverlayLifecycle owner;

            explicit OverlayRig(std::uint32_t layers = 8, std::uint32_t retired = 8, std::uint32_t previous = 0)
                : owner(std::move(UiOverlayLifecycle::Create({Owner(), layers, retired, previous})).Value()) {}

            UiOverlayLayerId Show(const LayerOptions &options, UiModalExclusivity exclusivity = UiModalExclusivity::None,
                                  UiOverlayViewportId viewport = {Owner(), 500, 1}) {
                auto publisher = Publisher(allocator, options);
                auto shown = owner.Show(Binding(options, exclusivity, viewport), std::move(publisher));
                REQUIRE(shown.HasValue());
                return shown.Value();
            }

            UiReloadCanvas &Layer(UiOverlayLayerId id) {
                auto *publisher = owner.Publisher(id);
                REQUIRE(publisher != nullptr);
                return Canvas(*publisher);
            }

            /** @brief Uncovering must restore both the input gate and the actual authored default focus. */
            void ExpectEligibleDefault(UiOverlayLayerId id) {
                CHECK(owner.InputStatus(id) == UiOverlayInputStatus::Eligible);
                const auto focused = Layer(id).focus->CurrentFocus();
                REQUIRE(focused.HasValue());
                REQUIRE(focused.Value().has_value());
                CHECK(focused.Value()->id == Stable<UiElementId>(11));
            }

            void Present(UiOverlayLayerId id, const LayerOptions &options, std::uint64_t snapshot = 1) {
                const auto &canvas = Layer(id);
                const UiPresentationReceipt receipt{options.view,
                                                    canvas.tree.Canvas(),
                                                    canvas.layout->Descriptor().interaction,
                                                    Revision<UiRenderSnapshotRevision>(snapshot),
                                                    UiPresentationOutcome::Presented,
                                                    UiPresentationReason::None};
                REQUIRE(owner.ApplyPresentation(id, receipt).HasValue());
            }

            UiPointerCaptureToken Capture(UiOverlayLayerId id, const LayerOptions &options) {
                auto &canvas = Layer(id);
                const auto target = canvas.controls.front().control.Element();
                const UiEventRoute route{canvas.tree.Instance(),
                                         canvas.tree.Canvas(),
                                         canvas.tree.SourceDocument(),
                                         canvas.tree.Revision(),
                                         canvas.layout->Descriptor().interaction,
                                         target,
                                         std::nullopt};
                const UiPointerCaptureRequest request{Binding(options).context, UiPointerId::Create(1).Value(), UiPointerButton::Primary,
                                                      options.view, route};
                auto captured = canvas.captures->Capture(request, canvas.tree, canvas.presentations.front());
                REQUIRE(captured.HasValue());
                return std::move(captured).Value();
            }
        };

        TEST_CASE("Overlay owner sorts real route priority and traps nested modal input and focus", "[runtime_ui][overlay][modal]") {
            OverlayRig rig;
            const auto screenOptions = Options(1);
            const auto screen = rig.Show(screenOptions);
            rig.Present(screen, screenOptions);
            REQUIRE(rig.owner.InputStatus(screen) == UiOverlayInputStatus::Eligible);
            REQUIRE(rig.Layer(screen).focus->CurrentFocus().Value().has_value());
            const auto modalOptions = Options(2, UiPresentationBand::Modal);
            const auto modal = rig.Show(modalOptions, UiModalExclusivity::Viewport);
            CHECK(rig.owner.InputStatus(screen) == UiOverlayInputStatus::Blocked);
            CHECK_FALSE(rig.Layer(screen).focus->CurrentFocus().Value().has_value());
            CHECK(rig.Layer(modal).focus->Snapshot().Value().modalDepth == 1);
            const auto topOptions = Options(3, UiPresentationBand::Modal);
            const auto top = rig.Show(topOptions, UiModalExclusivity::Viewport);
            CHECK(rig.owner.Dismiss(modal).HasError());
            std::array<UiOverlayLayerSnapshot, 3> snapshot;
            REQUIRE(rig.owner.Snapshot(snapshot).Value() == 3);
            CHECK(snapshot[0].layer == screen);
            CHECK(snapshot[1].layer == modal);
            CHECK(snapshot[2].layer == top);
            REQUIRE(rig.owner.Dismiss(top).HasValue());
            CHECK(rig.Layer(modal).focus->CurrentFocus().Value()->id == Stable<UiElementId>(11));
            REQUIRE(rig.owner.Dismiss(modal).HasValue());
            rig.ExpectEligibleDefault(screen);
        }

        TEST_CASE("Overlay exclusivity matches explicit viewport player or game instance audience", "[runtime_ui][overlay][scope]") {
            OverlayRig rig;
            auto one = Options(1);
            auto two = Options(2);
            two.player = UiFocusPlayerId{Owner(), 99, 1};
            two.view = {Owner(), 99, 1};
            const auto first = rig.Show(one);
            const auto second = rig.Show(two, UiModalExclusivity::None, {Owner(), 501, 1});
            rig.Present(first, one);
            rig.Present(second, two);
            auto modalOptions = Options(3, UiPresentationBand::Modal);
            UiModalExclusivity policy = UiModalExclusivity::Viewport;
            SECTION("Viewport") {}
            SECTION("Player across views") {
                modalOptions.view = two.view;
                policy = UiModalExclusivity::Player;
            }
            SECTION("Game instance") {
                policy = UiModalExclusivity::GameInstance;
            }
            const auto modal = rig.Show(modalOptions, policy);
            CHECK(rig.owner.InputStatus(first) == UiOverlayInputStatus::Blocked);
            CHECK(rig.owner.InputStatus(second) ==
                  (policy == UiModalExclusivity::GameInstance ? UiOverlayInputStatus::Blocked : UiOverlayInputStatus::Eligible));
            REQUIRE(rig.owner.Dismiss(modal).HasValue());
            CHECK(rig.owner.InputStatus(first) == UiOverlayInputStatus::Eligible);
        }

        TEST_CASE("Noninteractive overlay leaves lower context focus and input authoritative", "[runtime_ui][overlay]") {
            OverlayRig rig;
            auto screenOptions = Options(1);
            const auto screen = rig.Show(screenOptions);
            rig.Present(screen, screenOptions);
            auto overlayOptions = Options(2, UiPresentationBand::Overlay);
            auto publisher = Publisher(rig.allocator, overlayOptions);
            auto binding = Binding(overlayOptions);
            binding.interactive = false;
            const auto overlay = rig.owner.Show(binding, std::move(publisher)).Value();
            CHECK(rig.owner.InputStatus(overlay) == UiOverlayInputStatus::Blocked);
            rig.ExpectEligibleDefault(screen);
        }

        TEST_CASE("Opening a modal cancels actual lower-context capture exactly once", "[runtime_ui][overlay][capture]") {
            OverlayRig rig;
            const auto options = Options(1);
            const auto screen = rig.Show(options);
            rig.Present(screen, options);
            auto capture = rig.Capture(screen, options);
            REQUIRE(capture.IsActive());
            const auto modal = rig.Show(Options(2, UiPresentationBand::Modal), UiModalExclusivity::Viewport);
            CHECK_FALSE(capture.IsActive());
            CHECK(capture.CancellationReason() == UiPointerCaptureCancellationReason::ModalOpened);
            REQUIRE(rig.owner.Dismiss(modal).HasValue());
            CHECK(capture.CancellationReason() == UiPointerCaptureCancellationReason::ModalOpened);
            CHECK(rig.Layer(screen).captures->ActiveCount() == 0);
        }

        TEST_CASE("Modal close never restores a disabled prior focus target", "[runtime_ui][overlay][focus]") {
            OverlayRig rig;
            const auto screen = rig.Show(Options(1));
            const auto modal = rig.Show(Options(2, UiPresentationBand::Modal), UiModalExclusivity::GameInstance);
            auto &focus = *rig.Layer(screen).focus;
            const auto child = rig.Layer(screen).tree.Find(Stable<UiElementId>(11)).Value();
            REQUIRE(focus.SetParticipation(focus.Owner(), {child, true, false, true}).HasValue());
            REQUIRE(rig.owner.Dismiss(modal).HasValue());
            CHECK_FALSE(focus.CurrentFocus().Value().has_value());
        }

        TEST_CASE("Overlay admission failure preserves actual supplied publisher and existing barriers",
                  "[runtime_ui][overlay][validation]") {
            OverlayRig rig{1, 1};
            const auto original = rig.Show(Options(1));
            auto options = Options(2, UiPresentationBand::Modal);
            auto publisher = Publisher(rig.allocator, options);
            CHECK(rig.owner.Show(Binding(options, UiModalExclusivity::Viewport), std::move(publisher)).HasError());
            REQUIRE(publisher.Current() != nullptr);
            CHECK(Canvas(publisher).focus->Snapshot().Value().modalDepth == 0);
            CHECK(rig.owner.Publisher(original) != nullptr);
            REQUIRE(rig.owner.Dismiss(original).HasValue());
            auto malformed = Binding(options, UiModalExclusivity::Viewport);
            malformed.modalRoot = Stable<UiElementId>(99);
            CHECK(rig.owner.Show(malformed, std::move(publisher)).HasError());
            CHECK(publisher.Current() != nullptr);
            CHECK(rig.owner.LastIssuedLayerIncarnation() == original.generation);
        }

        TEST_CASE("Overlay reload preserves stable restoration and fails cancelled publication atomically",
                  "[runtime_ui][overlay][reload]") {
            OverlayRig rig;
            const auto options = Options(1);
            const auto screen = rig.Show(options);
            const auto modal = rig.Show(Options(2, UiPresentationBand::Modal), UiModalExclusivity::GameInstance);
            auto oldHandle = rig.Layer(screen).tree.Find(Stable<UiElementId>(11)).Value();
            CancellationSource cancellation;
            auto prepared =
                rig.owner.PrepareReload(screen, Generation(rig.allocator, 2, 32, false, 0, 0, 3, options), cancellation.Token());
            REQUIRE(prepared.HasValue());
            auto candidate = std::move(prepared).Value();
            cancellation.RequestCancellation();
            CHECK(rig.owner.CommitReload(screen, candidate, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasError());
            CHECK(rig.Layer(screen).tree.Find(Stable<UiElementId>(11)).Value() == oldHandle);
            auto next = rig.owner.PrepareReload(screen, Generation(rig.allocator, 3, 32, false, 0, 0, 3, options));
            REQUIRE(next.HasValue());
            auto replacement = std::move(next).Value();
            REQUIRE(rig.owner.CommitReload(screen, replacement, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
            CHECK(rig.Layer(screen).tree.Get(oldHandle).HasError());
            REQUIRE(rig.owner.Dismiss(modal).HasValue());
            CHECK(rig.Layer(screen).focus->CurrentFocus().Value()->id == Stable<UiElementId>(11));
        }

        TEST_CASE("Overlay suspension and invalid route mutation fail closed without partial snapshot output",
                  "[runtime_ui][overlay][lifecycle]") {
            OverlayRig rig;
            const auto options = Options(1);
            const auto screen = rig.Show(options);
            rig.Present(screen, options);
            REQUIRE(rig.owner.SetSuspended(true).HasValue());
            CHECK(rig.owner.InputStatus(screen) == UiOverlayInputStatus::Suspended);
            REQUIRE(rig.owner.SetSuspended(false).HasValue());
            CHECK(rig.owner.InputStatus(screen) == UiOverlayInputStatus::AwaitingPresentation);
            const auto &canvas = rig.Layer(screen);
            const UiPresentationReceipt replay{options.view,
                                               canvas.tree.Canvas(),
                                               canvas.layout->Descriptor().interaction,
                                               Revision<UiRenderSnapshotRevision>(1),
                                               UiPresentationOutcome::Presented,
                                               UiPresentationReason::None};
            CHECK(rig.owner.ApplyPresentation(screen, replay).HasError());
            CHECK(rig.owner.InputStatus(screen) == UiOverlayInputStatus::AwaitingPresentation);
            rig.Present(screen, options, 2);
            CHECK(rig.owner.InputStatus(screen) == UiOverlayInputStatus::Eligible);
            auto other = rig.Show(Options(2));
            rig.owner.Publisher(other)->Shutdown();
            std::array<UiOverlayLayerSnapshot, 2> snapshot;
            snapshot[0].layer = screen;
            snapshot[0].order = 777;
            CHECK(rig.owner.Snapshot(snapshot).HasError());
            CHECK(snapshot[0].order == 777);
            REQUIRE(rig.Layer(screen).routes->Pop().Value().IsCommitted());
            CHECK(rig.owner.InputStatus(screen) == UiOverlayInputStatus::Retired);
        }

        TEST_CASE("Modal restoration after cooked target removal chooses the new authored default", "[runtime_ui][overlay][reload]") {
            OverlayRig rig;
            auto options = Options(1);
            const auto screen = rig.Show(options);
            const auto modal = rig.Show(Options(2, UiPresentationBand::Modal), UiModalExclusivity::GameInstance);
            options.childMarker = 44;
            auto prepared = rig.owner.PrepareReload(screen, Generation(rig.allocator, 2, 32, false, 0, 0, 3, options));
            REQUIRE(prepared.HasValue());
            auto candidate = std::move(prepared).Value();
            REQUIRE(rig.owner.CommitReload(screen, candidate, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
            CHECK(rig.Layer(screen).tree.Find(Stable<UiElementId>(11)).HasError());
            REQUIRE(rig.owner.Dismiss(modal).HasValue());
            CHECK(rig.Layer(screen).focus->CurrentFocus().Value()->id == Stable<UiElementId>(44));
        }

        TEST_CASE("Overlay identities survive moves without ABA and exhausted admission preserves private generation",
                  "[runtime_ui][overlay][aba]") {
            OverlayRig rig;
            const auto original = rig.Show(Options(1));
            auto moved = std::move(rig.owner);
            CHECK(rig.owner.State() == UiOverlayLifecycleState::Stopped);
            CHECK(rig.owner.InputStatus(original) == UiOverlayInputStatus::Retired);
            CHECK(rig.owner.Publisher(original) == nullptr);
            REQUIRE(moved.Dismiss(original).HasValue());
            auto publisher = Publisher(rig.allocator, Options(2));
            const auto replacement = moved.Show(Binding(Options(2)), std::move(publisher)).Value();
            CHECK(replacement != original);
            CHECK(moved.Dismiss(original).HasError());
            OverlayRig exhausted{1, 1, std::numeric_limits<std::uint32_t>::max()};
            auto unused = Publisher(exhausted.allocator, Options(3));
            CHECK(exhausted.owner.Show(Binding(Options(3)), std::move(unused)).HasError());
            CHECK(unused.Current() != nullptr);
            CHECK(exhausted.owner.LastIssuedLayerIncarnation() == std::numeric_limits<std::uint32_t>::max());
        }

        TEST_CASE("Overlay dismissal uses reserved slots and shutdown retains actual leases", "[runtime_ui][overlay][allocation]") {
            OverlayRig rig{2, 1};
            auto options = Options(1);
            auto screen = rig.Show(options);
            std::optional<UiReloadLease> lease{rig.owner.Publisher(screen)->Acquire().Value()};
            auto modalPublisher = Publisher(rig.allocator, Options(2, UiPresentationBand::Modal));
            auto binding = Binding(Options(2, UiPresentationBand::Modal), UiModalExclusivity::GameInstance);
            const auto before = Tests::AllocationProbe::Count();
            const auto frees = Tests::AllocationProbe::FreeCount();
            const auto shown = rig.owner.Show(binding, std::move(modalPublisher));
            const auto dismissed = rig.owner.Dismiss(shown.Value());
            const auto after = Tests::AllocationProbe::Count();
            const auto freed = Tests::AllocationProbe::FreeCount();
            REQUIRE(shown.HasValue());
            REQUIRE(dismissed.HasValue());
            CHECK(before == after);
            CHECK(frees == freed);
            CHECK(rig.owner.Dismiss(screen).HasError());
            rig.owner.Shutdown();
            rig.owner.Shutdown();
            CHECK_FALSE(rig.owner.CanReclaim());
            CHECK(rig.owner.CollectRetired().HasValue());
            CHECK_FALSE(rig.owner.CanReclaim());
            lease.reset();
            CHECK(rig.owner.CollectRetired().HasValue());
            CHECK(rig.owner.CanReclaim());
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui::ReloadTests
