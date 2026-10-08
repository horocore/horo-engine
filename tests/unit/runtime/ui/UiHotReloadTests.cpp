#include "UiHotReloadTestFixture.h"
#include "support/AllocationProbe.h"

#include <array>
#include <limits>

namespace Horo::Runtime::Ui::ReloadTests {
    namespace {
        UiHotReload Publisher(UiElementSlotAllocator &allocator, UiHotReloadLimits limits = {}) {
            return std::move(UiHotReload::Create(Generation(allocator, 1), limits)).Value();
        }

        UiHotReload::Prepared Prepare(UiHotReload &publisher, UiElementSlotAllocator &allocator, std::uint64_t version,
                                      std::uint16_t textLimit = 32) {
            auto candidate = publisher.Prepare(Generation(allocator, version, textLimit));
            REQUIRE(candidate.HasValue());
            return std::move(candidate).Value();
        }

        void Publish(UiHotReload &publisher, UiHotReload::Prepared &prepared) {
            REQUIRE(publisher.Commit(prepared, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
        }

        UiTextInputControlState State(const UiReloadCanvas &canvas) {
            return std::get<UiTextInputControlState>(canvas.controls.front().control.Snapshot().Value());
        }

        UiFocusModalActivation Modal(UiReloadCanvas &canvas) {
            const auto root = canvas.tree.Root().Value();
            auto activation = canvas.focus->PushModal({root.handle, root.id, canvas.controls.front().id});
            REQUIRE(activation.HasValue());
            return activation.Value();
        }

        TEST_CASE("UI reload preserves compatible draft focus route and scroll through actual owners", "[runtime_ui][reload]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator);
            auto &old = Canvas(publisher);
            Draft(old, " draft");
            REQUIRE(old.routes->Push(Stable<UiRouteId>(9)).Value().IsCommitted());
            old.clipPolicies.front().scrollOffset = {180, 180};
            old.clipped = std::move(old.clipping->Update(old.tree, *old.layout, {old.clipPolicies, {}})).Value();
            REQUIRE(old.clipped->Scrolls().front().maximumOffset == UiLogicalPoint{200, 200});
            REQUIRE(old.clipped->Scrolls().front().offset == UiLogicalPoint{180, 180});
            auto prepared = Prepare(publisher, allocator, 2);
            CHECK(prepared.Reconciliation().preservedControls == 1);
            CHECK(prepared.Reconciliation().preservedFocus == 1);
            CHECK(prepared.Reconciliation().preservedRoutes == 1);
            CHECK(prepared.Reconciliation().preservedScrolls == 1);
            Publish(publisher, prepared);
            auto &current = Canvas(publisher);
            CHECK(State(current).text.View() == "base draft");
            CHECK(State(current).editing);
            CHECK(current.routes->Top()->metadata.id == Stable<UiRouteId>(9));
            REQUIRE(current.clipped->Scrolls().front().maximumOffset == UiLogicalPoint{50, 50});
            CHECK(current.clipped->Scrolls().front().offset == UiLogicalPoint{50, 50});
            auto &control = current.controls.front().control;
            REQUIRE(control.Handle(Input(control, UiControlInputKind::Cancel, 1)).HasValue());
            CHECK(State(current).text.View() == "base");
        }

        TEST_CASE("UI reload narrowing text limits retains authored fallback without truncating drafts", "[runtime_ui][reload]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator);
            Draft(Canvas(publisher), " draft");
            auto prepared = Prepare(publisher, allocator, 2, 4);
            CHECK(prepared.Reconciliation().preservedControls == 0);
            CHECK(prepared.Reconciliation().resetControls == 1);
            Publish(publisher, prepared);
            CHECK(State(Canvas(publisher)).text.View() == "base");
            CHECK_FALSE(State(Canvas(publisher)).editing);
        }

        TEST_CASE("UI reload rejects a candidate after an intervening actual draft edit", "[runtime_ui][reload][stale]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator);
            Draft(Canvas(publisher), " first");
            auto prepared = Prepare(publisher, allocator, 2);
            auto &control = Canvas(publisher).controls.front().control;
            REQUIRE(control.Handle(Input(control, UiControlInputKind::TextInput, 3, " second")).HasValue());
            const auto result = publisher.Commit(prepared, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands);
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == UiErrors::RevisionStale.code.Value());
            CHECK(State(Canvas(publisher)).text.View() == "base first second");
            CHECK(publisher.Current()->Instance().DocumentRevision() == Revision<UiDocumentRevision>(1));
        }

        TEST_CASE("UI reload detects replaced control contracts despite identical displayed values", "[runtime_ui][reload][stale]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator);
            auto prepared = Prepare(publisher, allocator, 2);
            auto &control = Canvas(publisher).controls.front().control;
            const UiTextInputControlDescriptor changed{{control.Owner(), control.Element(), Stable<UiActionId>(99), {}, true, true, {}},
                                                       Text("base"),
                                                       32,
                                                       true};
            control = std::move(UiControlStateMachine::Create(changed)).Value();
            const auto result = publisher.Commit(prepared, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands);
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == UiErrors::RevisionStale.code.Value());
            CHECK(State(Canvas(publisher)).text.View() == "base");
        }

        TEST_CASE("UI reload rejects prepared source route or scroll mutation atomically", "[runtime_ui][reload][stale]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator);
            auto prepared = Prepare(publisher, allocator, 2);
            SECTION("route revision") {
                REQUIRE(Canvas(publisher).routes->Push(Stable<UiRouteId>(9)).Value().IsCommitted());
            }
            SECTION("scroll at unchanged layout revision") {
                auto &canvas = Canvas(publisher);
                canvas.clipPolicies.front().scrollOffset = {30, 30};
                canvas.clipped = std::move(canvas.clipping->Update(canvas.tree, *canvas.layout, {canvas.clipPolicies, {}})).Value();
            }
            const auto result = publisher.Commit(prepared, UiStructuralCommitPoint::CommitDeferredLifecycleChanges);
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == UiErrors::RevisionStale.code.Value());
            CHECK(publisher.Current()->Instance().DocumentRevision() == Revision<UiDocumentRevision>(1));
        }

        TEST_CASE("UI reload rejects old raw element modal route and presentation identities", "[runtime_ui][reload][aba]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator);
            auto &old = Canvas(publisher);
            const auto element = old.controls.front().control.Element();
            const auto modal = Modal(old).modal;
            REQUIRE(old.focus->PopModal(modal).HasValue());
            const auto route = old.routes->Push(Stable<UiRouteId>(9)).Value().route.value();
            REQUIRE(old.routes->Pop().Value().IsCommitted());
            const UiPresentationReceipt receipt{{Owner(), 10, 1},
                                                old.tree.Canvas(),
                                                Revision<UiInteractionRevision>(1),
                                                Revision<UiRenderSnapshotRevision>(1),
                                                UiPresentationOutcome::Presented,
                                                UiPresentationReason::None};
            REQUIRE(publisher.ApplyPresentation(old.id, receipt).Value());
            CHECK(publisher.InputEligible(old.id, receipt.view));
            auto prepared = Prepare(publisher, allocator, 2);
            Publish(publisher, prepared);
            auto &current = Canvas(publisher);
            const auto newModal = Modal(current).modal;
            REQUIRE(current.routes->Push(Stable<UiRouteId>(9)).Value().IsCommitted());
            CHECK(newModal != modal);
            CHECK(current.tree.Get(element).HasError());
            CHECK(current.focus->PopModal(modal).HasError());
            CHECK(current.routes->Actions(route) == nullptr);
            const auto refused = current.routes->Pop({current.routes->Stack(), current.routes->Revision(), route});
            REQUIRE(refused.HasValue());
            CHECK(refused.Value().rejection == UiRouteOperationRejection::GuardMismatch);
            CHECK(publisher.ApplyPresentation(current.id, receipt).HasError());
            CHECK_FALSE(publisher.InputEligible(current.id, receipt.view));
        }

        TEST_CASE("UI reload modal and route high water reject deep ABA and overflow without mutation", "[runtime_ui][reload][aba]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto generation = Generation(allocator, 1, 32, false, std::numeric_limits<std::uint32_t>::max() - 2,
                                         std::numeric_limits<std::uint32_t>::max() - 2);
            auto publisher = std::move(UiHotReload::Create(std::move(generation))).Value();
            auto &canvas = Canvas(publisher);
            const auto first = Modal(canvas).modal;
            const auto second = Modal(canvas).modal;
            REQUIRE(canvas.focus->PopModal(second).HasValue());
            REQUIRE(canvas.focus->PopModal(first).HasValue());
            CHECK(canvas.focus->PushModal({canvas.tree.Root().Value().handle}).HasError());
            CHECK(canvas.focus->Snapshot().Value().modalDepth == 0);
            const auto one = canvas.routes->Push(Stable<UiRouteId>(9)).Value().route.value();
            REQUIRE(canvas.routes->Pop().Value().IsCommitted());
            const auto two = canvas.routes->Push(Stable<UiRouteId>(9)).Value().route.value();
            CHECK(one != two);
            const auto revision = canvas.routes->Revision();
            CHECK(canvas.routes->Replace(Stable<UiRouteId>(9)).HasError());
            CHECK(canvas.routes->Revision() == revision);
            CHECK(canvas.routes->Top()->id == two);
        }

        TEST_CASE("UI reload namespace exhaustion never wraps or modifies the active generation", "[runtime_ui][reload][capacity]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner(), std::numeric_limits<std::uint32_t>::max() - 8)).Value();
            auto publisher = Publisher(allocator);
            auto &canvas = Canvas(publisher);
            const auto revision = canvas.tree.Revision();
            const std::array elements{UiElementDescriptor{Stable<UiElementId>(10), {}},
                                      UiElementDescriptor{Stable<UiElementId>(11), Stable<UiElementId>(10)}};
            const auto exhausted = UiElementTree::Create(allocator,
                                                         {Instance(),
                                                          canvas.tree.Canvas(),
                                                          Stable<UiDocumentId>(1),
                                                          Revision<UiDocumentRevision>(2),
                                                          Revision<UiRuntimeTreeRevision>(2),
                                                          {8, 8, 8}},
                                                         elements);
            REQUIRE(exhausted.HasError());
            CHECK(exhausted.ErrorValue().code.Value() == UiErrors::GenerationExhausted.code.Value());
            CHECK(canvas.tree.Revision() == revision);
            CHECK(canvas.tree.State() == UiElementTreeState::Active);
            CHECK(publisher.Current()->Instance().DocumentRevision() == Revision<UiDocumentRevision>(1));
        }

        TEST_CASE("UI reload burns failed candidate namespaces and rejects a restarted allocator", "[runtime_ui][reload][aba]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator);
            CancellationSource cancellation;
            cancellation.RequestCancellation();
            CHECK(publisher.Prepare(Generation(allocator, 2), cancellation.Token()).HasError());
            auto restarted = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            const auto reused = publisher.Prepare(Generation(restarted, 3));
            REQUIRE(reused.HasError());
            CHECK(reused.ErrorValue().code.Value() == UiErrors::HandleStale.code.Value());
            auto prepared = Prepare(publisher, allocator, 4);
            Publish(publisher, prepared);
            CHECK(publisher.Current()->Instance().DocumentRevision() == Revision<UiDocumentRevision>(4));
        }

        TEST_CASE("UI reload commit uses reserved retention with zero allocation or final free", "[runtime_ui][reload][allocation]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator);
            auto old = publisher.Acquire().Value();
            auto prepared = Prepare(publisher, allocator, 2);
            const auto before = Tests::AllocationProbe::Count();
            const auto frees = Tests::AllocationProbe::FreeCount();
            auto result = publisher.Commit(prepared, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands);
            const auto after = Tests::AllocationProbe::Count();
            const auto freed = Tests::AllocationProbe::FreeCount();
            REQUIRE(result.HasValue());
            CHECK(after == before);
            CHECK(freed == frees);
            CHECK_FALSE(publisher.IsCurrent(old));
            CHECK(old.Get()->Instance().State() == UiRuntimeInstanceState::Retiring);
            CHECK(publisher.CollectRetired().Value() == 0);
        }

        TEST_CASE("UI reload cancellation stale candidates and shutdown preserve held generations", "[runtime_ui][reload][lifecycle]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator);
            auto old = publisher.Acquire().Value();
            CancellationSource cancellation;
            auto cancelled = publisher.Prepare(Generation(allocator, 2), cancellation.Token());
            REQUIRE(cancelled.HasValue());
            auto cancelledCandidate = std::move(cancelled).Value();
            cancellation.RequestCancellation();
            CHECK(publisher.Commit(cancelledCandidate, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasError());
            auto first = Prepare(publisher, allocator, 3);
            auto second = Prepare(publisher, allocator, 4);
            Publish(publisher, first);
            CHECK(publisher.Commit(second, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasError());
            publisher.Shutdown();
            CHECK(publisher.Acquire().HasError());
            CHECK_FALSE(publisher.IsCurrent(old));
            CHECK_FALSE(publisher.CanReclaim());
            CHECK(old.Get()->Instance().Payload().size() > 0);
            CHECK(publisher.Commit(first, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasError());
        }

        TEST_CASE("UI reload public candidate moves release each prepared reservation exactly once", "[runtime_ui][reload][lifecycle]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            SECTION("move construction and abandonment release one reservation") {
                auto publisher = Publisher(allocator, {2, 1});
                std::optional<UiHotReload::Prepared> retained;
                {
                    auto original = Prepare(publisher, allocator, 2);
                    retained.emplace(std::move(original));
                }
                const auto full = publisher.Prepare(Generation(allocator, 3));
                REQUIRE(full.HasError());
                CHECK(full.ErrorValue().code.Value() == UiErrors::CapacityExceeded.code.Value());
                retained.reset();
                auto next = publisher.Prepare(Generation(allocator, 4));
                REQUIRE(next.HasValue());
            }
            SECTION("move assignment abandons the old reservation and transfers the replacement") {
                auto publisher = Publisher(allocator, {2, 2});
                std::optional<UiHotReload::Prepared> retained;
                {
                    auto original = Prepare(publisher, allocator, 2);
                    auto replacement = Prepare(publisher, allocator, 3);
                    original = std::move(replacement);
                    retained.emplace(std::move(original));
                }
                auto next = publisher.Prepare(Generation(allocator, 4));
                REQUIRE(next.HasValue());
                const auto full = publisher.Prepare(Generation(allocator, 5));
                REQUIRE(full.HasError());
                CHECK(full.ErrorValue().code.Value() == UiErrors::CapacityExceeded.code.Value());
                retained.reset();
                auto afterAbandonment = publisher.Prepare(Generation(allocator, 6));
                REQUIRE(afterAbandonment.HasValue());
            }
        }

        TEST_CASE("UI reload publishes every cooked canvas as one generation", "[runtime_ui][reload][atomic]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = std::move(UiHotReload::Create(Generation(allocator, 1, 32, true))).Value();
            Draft(Canvas(publisher), " one");
            Draft(*publisher.Current()->Canvas(Stable<UiCanvasId>(3)), " two");
            auto prepared = publisher.Prepare(Generation(allocator, 2, 32, true));
            REQUIRE(prepared.HasValue());
            auto candidate = std::move(prepared).Value();
            CHECK(candidate.Reconciliation().preservedControls == 2);
            Publish(publisher, candidate);
            CHECK(State(Canvas(publisher)).text.View() == "base one");
            CHECK(State(*publisher.Current()->Canvas(Stable<UiCanvasId>(3))).text.View() == "base two");
        }

        TEST_CASE("UI reload never revives retired modal namespaces across repeated generations", "[runtime_ui][reload][aba]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator, {32, 4});
            const auto ancient = Modal(Canvas(publisher)).modal;
            REQUIRE(Canvas(publisher).focus->PopModal(ancient).HasValue());
            for (std::uint64_t version = 2; version < 18; ++version) {
                auto prepared = Prepare(publisher, allocator, version);
                Publish(publisher, prepared);
                auto &canvas = Canvas(publisher);
                const auto first = Modal(canvas).modal;
                const auto deep = Modal(canvas).modal;
                REQUIRE(canvas.focus->PopModal(deep).HasValue());
                CHECK(canvas.focus->PopModal(ancient).HasError());
                REQUIRE(canvas.focus->PopModal(first).HasValue());
                (void)publisher.CollectRetired();
            }
        }

        TEST_CASE("UI reload refuses a foreign actual focus audience instead of fabricating a rebind", "[runtime_ui][reload][scope]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator);
            auto replacement = Generation(allocator, 2);
            auto &canvas = *replacement.Canvas(Stable<UiCanvasId>(2));
            auto owner = canvas.focus->Owner();
            owner.scope.presentationLayer = {Owner(), 99, 1};
            const auto root = canvas.tree.Root().Value();
            const auto child = canvas.tree.Get(canvas.controls.front().control.Element()).Value();
            const std::array
                nodes{UiFocusNodeDescriptor{root.handle, root.id, {}, {}, UiFocusBringIntoViewPolicy::Nearest, false, true, true},
                      UiFocusNodeDescriptor{child.handle, child.id, root.id, {}, UiFocusBringIntoViewPolicy::Nearest, true, true, true}};
            canvas.focus.emplace(
                std::move(UiFocusGraph::Create({owner, child.id, UiFocusRecoveryPolicy::AncestorThenDefaultThenFirst, 8, 4, 4}, nodes))
                    .Value());
            const auto rejected = publisher.Prepare(std::move(replacement));
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == UiErrors::HandleOwnerMismatch.code.Value());
            CHECK(Canvas(publisher).focus->Owner().scope.presentationLayer.slot == 4);
            CHECK(publisher.Current()->Instance().DocumentRevision() == Revision<UiDocumentRevision>(1));
        }

        TEST_CASE("UI reload invalid owner mutation and malformed replacement retain last known good", "[runtime_ui][reload][validation]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator);
            auto replacement = Generation(allocator, 2);
            replacement.Canvas(Stable<UiCanvasId>(2))->controls.front().id = Stable<UiElementId>(99);
            const auto result = publisher.Prepare(std::move(replacement));
            REQUIRE(result.HasError());
            CHECK(publisher.Current()->Instance().DocumentRevision() == Revision<UiDocumentRevision>(1));
            CHECK(Canvas(publisher).tree.State() == UiElementTreeState::Active);
        }

        TEST_CASE("UI reload initial admission revalidates detached canonical owner composition", "[runtime_ui][reload][validation]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto initial = Generation(allocator, 1);
            initial.Canvas(Stable<UiCanvasId>(2))->controls.front().id = Stable<UiElementId>(99);
            const auto refused = UiHotReload::Create(std::move(initial));
            REQUIRE(refused.HasError());
            CHECK(refused.ErrorValue().code.Value() == UiErrors::DocumentInvalid.code.Value());
        }

        TEST_CASE("UI reload shutdown waits for pinned layout and generation leases to drain", "[runtime_ui][reload][lifecycle]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator);
            std::optional<UiReloadLease> lease{publisher.Acquire().Value()};
            std::optional<UiLayoutSnapshot> layout{*Canvas(publisher).layout};
            publisher.Shutdown();
            CHECK(publisher.CollectRetired().Value() == 0);
            lease.reset();
            CHECK(publisher.CollectRetired().Value() == 0);
            CHECK_FALSE(publisher.CanReclaim());
            layout.reset();
            CHECK(publisher.CollectRetired().Value() == 1);
            CHECK(publisher.CanReclaim());
        }

        TEST_CASE("UI reload retention capacity fails atomically and collection admits the next generation",
                  "[runtime_ui][reload][capacity]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            auto publisher = Publisher(allocator, {1, 2});
            {
                auto first = Prepare(publisher, allocator, 2);
                Publish(publisher, first);
            }
            auto second = Prepare(publisher, allocator, 3);
            CHECK(publisher.Commit(second, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasError());
            CHECK(publisher.Current()->Instance().DocumentRevision() == Revision<UiDocumentRevision>(2));
            CHECK(publisher.CollectRetired().Value() == 1);
            Publish(publisher, second);
            CHECK(publisher.Current()->Instance().DocumentRevision() == Revision<UiDocumentRevision>(3));
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui::ReloadTests
