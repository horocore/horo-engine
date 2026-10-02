#include "UiAccessibilityTestUtils.h"

namespace Horo::Runtime::Ui {
    namespace {
        UiFocusGraph Focus(const UiElementTree &tree, const std::uint32_t layer = 3) {
            const auto semantic = Descriptor(tree);
            const UiFocusOwnerContext owner{semantic.instance,         semantic.canvas,       semantic.document,
                                            semantic.documentRevision, semantic.treeRevision, semantic.interactionRevision,
                                            {{}, {Owner(), layer, 1}}};
            std::array<UiFocusNodeDescriptor, 5> nodes{};
            const std::array<std::uint8_t, 5> order{1, 5, 4, 3, 2};
            for (std::size_t index = 0; index < order.size(); ++index) {
                auto &node = nodes[index];
                node.id = Stable<UiElementId>(order[index]);
                node.element = tree.Find(node.id).Value();
                const auto parent = Stable<UiElementId>(order[index] == 4 ? 5 : 1);
                node.parent = order[index] == 1 ? UiElementId{} : parent;
                node.focusable = order[index] == 3 || order[index] == 4;
            }
            nodes[2].links.targets[0] = nodes[3].id;
            const auto descriptor =
                UiFocusGraphDescriptor{owner, Stable<UiElementId>(4), UiFocusRecoveryPolicy::AncestorThenDefaultThenFirst, 8, 4, 4};
            auto graph = UiFocusGraph::Create(descriptor, nodes);
            REQUIRE(graph.HasValue());
            return std::move(graph).Value();
        }

        UiAccessibilityAnnouncementInput Status(const UiAccessibilitySnapshot &snapshot, const std::uint64_t occurrence,
                                                const std::string_view text = "Ready") {
            return {{occurrence},
                    snapshot.Find(Stable<UiElementId>(4)).Value(),
                    UiAccessibilityAnnouncementPolicy::Polite,
                    {text, UiAccessibilityTextSource::ResolvedMessage},
                    UiAccessibilityAnnouncementKind::Status};
        }

        TEST_CASE("Accessibility reading and focus orders use the actual retained tree and interaction graph",
                  "[runtime_ui][accessibility][focus]") {
            auto tree = MakeTree();
            auto graph = Focus(tree);
            auto extractor = MakeExtractor(tree, 3);
            ProjectionFixture fixture;
            fixture.nodes[2].state = UiAccessibilityStateFlag::Focusable | UiAccessibilityStateFlag::Focused;
            accessibilityAllocations.store(0);
            const auto result = extractor.Extract(tree, Descriptor(tree), fixture.View(), graph);
            const auto allocations = accessibilityAllocations.load();
            REQUIRE(result.HasValue());
            REQUIRE(allocations == 0);
            const auto first = result.Value();
            REQUIRE(first.ReadingOrder().size() == 4);
            REQUIRE(first.FocusOrder().size() == 2);
            REQUIRE(first.FocusOrder()[0] == first.Find(Stable<UiElementId>(4)).Value());
            REQUIRE(first.FocusOrder()[1] == first.Find(Stable<UiElementId>(3)).Value());
            REQUIRE(first.Get(first.FocusOrder()[0]).Value().state.Has(UiAccessibilityStateFlag::Focused));
            REQUIRE_FALSE(first.Get(first.FocusOrder()[1]).Value().state.Has(UiAccessibilityStateFlag::Focused));
            REQUIRE(*first.FocusOwner() == graph.Owner());
            REQUIRE(graph.Move(UiNavigationDirection::Next).HasValue());
            const auto moved = extractor.Extract(tree, Descriptor(tree, 2), fixture.View(), graph).Value();
            REQUIRE(moved.Get(moved.FocusOrder()[1]).Value().state.Has(UiAccessibilityStateFlag::Focused));
            REQUIRE(first.Get(first.FocusOrder()[0]).Value().state.Has(UiAccessibilityStateFlag::Focused));

            const auto modal = graph.PushModal({tree.Find(Stable<UiElementId>(5)).Value(), Stable<UiElementId>(5), Stable<UiElementId>(4)});
            REQUIRE(modal.HasValue());
            const auto covered = extractor.Extract(tree, Descriptor(tree, 3), fixture.View(), graph).Value();
            REQUIRE(covered.FocusState()->activeModal == modal.Value().modal);
            REQUIRE(covered.ReadingOrder().size() == 1);
            REQUIRE(covered.FocusOrder().size() == 1);
            REQUIRE(covered.Get(covered.Find(Stable<UiElementId>(3)).Value()).Value().exposure == UiAccessibilityExposure::Covered);
            graph.Shutdown();
            extractor.Close();
            tree.Shutdown();
            REQUIRE(first.ReadingOrder().size() == 4);
            REQUIRE(first.FocusOrder().size() == 2);
            REQUIRE(covered.ReadingOrder().size() == 1);
        }

        TEST_CASE("Accessibility rejects stale audience and contradictory focus candidates before publication",
                  "[runtime_ui][accessibility][focus]") {
            auto tree = MakeTree();
            auto graph = Focus(tree);
            auto extractor = MakeExtractor(tree);
            ProjectionFixture fixture;
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View(), graph).Value();
            SECTION("another player layer cannot replace the bound audience") {
                auto other = Focus(tree, 4);
                RequireError(extractor.Extract(tree, Descriptor(tree, 2), fixture.View(), other),
                             UiErrors::AccessibilitySnapshotSourceStale);
            }
            SECTION("old presented interaction revision") {
                auto descriptor = Descriptor(tree, 2);
                descriptor.interactionRevision = UiInteractionRevision::Create(6).Value();
                RequireError(extractor.Extract(tree, descriptor, fixture.View(), graph), UiErrors::AccessibilitySnapshotSourceStale);
            }
            SECTION("disabled actual focus target") {
                fixture.nodes[3].state = UiAccessibilityState{UiAccessibilityStateFlag::Disabled};
                RequireError(extractor.Extract(tree, Descriptor(tree, 2), fixture.View(), graph), UiErrors::AccessibilityStateInvalid);
            }
            SECTION("hidden actual focus target") {
                fixture.nodes[3].exposure = UiAccessibilityExposure::Hidden;
                RequireError(extractor.Extract(tree, Descriptor(tree, 2), fixture.View(), graph), UiErrors::AccessibilityStateInvalid);
            }
            SECTION("missing actual focus target") {
                RequireError(extractor.Extract(tree, Descriptor(tree, 2), {std::span{fixture.nodes}.first(3)}, graph),
                             UiErrors::AccessibilitySnapshotSourceStale);
            }
            SECTION("stopped graph") {
                graph.Shutdown();
                REQUIRE(extractor.Extract(tree, Descriptor(tree, 2), fixture.View(), graph).HasError());
            }
            REQUIRE(first.FocusOrder().size() == 2);
        }

        TEST_CASE("Focus order capacity failure leaves caller output unchanged", "[runtime_ui][accessibility][focus]") {
            auto tree = MakeTree();
            auto graph = Focus(tree);
            std::array<UiFocusTarget, 1> output{UiFocusTarget{Stable<UiElementId>(1), tree.Root().Value().handle}};
            const auto previous = output[0];
            RequireError(graph.Order(output), UiErrors::FocusCapacityExceeded);
            REQUIRE(output[0] == previous);
            REQUIRE(graph.ClearFocus().HasValue());
            std::array<UiFocusTarget, 2> enough{};
            REQUIRE(graph.Order(enough).Value() == 2);
        }

        TEST_CASE("Accepted status occurrences remain FIFO across rapid updates and semantic resync",
                  "[runtime_ui][accessibility][delivery]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree, 4);
            auto publisher = MakePublisher(tree, {1, 2, 16, 0});
            ProjectionFixture fixture;
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            auto event = Status(first, 1);
            REQUIRE(publisher.Publish(first, std::span{&event, 1}).HasValue());
            REQUIRE(publisher.Status() == UiAccessibilityChangeStatus::Resynchronize);
            const auto cursor = publisher.Announcements()[0].cursor;
            event.text.text = "Busy";
            const auto second = extractor.Extract(tree, Descriptor(tree, 2), fixture.View()).Value();
            REQUIRE(publisher.Publish(second, std::span{&event, 1}).HasValue());
            REQUIRE(publisher.Announcements().size() == 1);
            REQUIRE(publisher.AnnouncementText(cursor) == "Ready");
            event.id = {2};
            const auto third = extractor.Extract(tree, Descriptor(tree, 3), fixture.View()).Value();
            REQUIRE(publisher.Publish(third, std::span{&event, 1}).HasValue());
            REQUIRE(publisher.Announcements().size() == 2);
            const auto newest = publisher.Announcements()[1].cursor;
            REQUIRE(newest.sequence == cursor.sequence + 1);
            REQUIRE(publisher.AnnouncementText(newest) == "Busy");
            event.id = {3};
            const auto fourth = extractor.Extract(tree, Descriptor(tree, 4), fixture.View()).Value();
            RequireError(publisher.Publish(fourth, std::span{&event, 1}), UiErrors::CapacityExceeded);
            REQUIRE(publisher.Revision() == third.Descriptor().semanticRevision);
            REQUIRE(publisher.Announcements().size() == 2);
            auto future = newest;
            ++future.sequence;
            RequireError(publisher.Acknowledge(future), UiErrors::AccessibilitySnapshotSourceStale);
            auto foreign = newest;
            foreign.generation = UiAccessibilityAnnouncementGeneration::Create(newest.generation.Value() + 1).Value();
            RequireError(publisher.Acknowledge(foreign), UiErrors::AccessibilitySnapshotSourceStale);
            REQUIRE(publisher.Acknowledge(cursor).HasValue());
            REQUIRE(publisher.Acknowledge(cursor).HasValue());
            REQUIRE(publisher.AnnouncementText(cursor).empty());
            REQUIRE(publisher.AnnouncementText(newest) == "Busy");
            REQUIRE(publisher.Publish(fourth, std::span{&event, 1}).HasValue());
            REQUIRE(publisher.Announcements().back().id == event.id);
            REQUIRE(publisher.Acknowledge(publisher.Announcements().back().cursor).HasValue());
            REQUIRE(publisher.Announcements().empty());
        }

        TEST_CASE("Delivery byte capacity rejects publication atomically until acknowledgment", "[runtime_ui][accessibility][delivery]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree);
            auto publisher = MakePublisher(tree, {16, 4, 5, 0});
            ProjectionFixture fixture;
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            auto event = Status(first, 1);
            REQUIRE(publisher.Publish(first, std::span{&event, 1}).HasValue());
            const auto cursor = publisher.Announcements()[0].cursor;
            event.id = {2};
            const auto second = extractor.Extract(tree, Descriptor(tree, 2), fixture.View()).Value();
            RequireError(publisher.Publish(second, std::span{&event, 1}), UiErrors::CapacityExceeded);
            REQUIRE(publisher.Revision() == first.Descriptor().semanticRevision);
            REQUIRE(publisher.AnnouncementText(cursor) == "Ready");
            REQUIRE(publisher.Acknowledge(cursor).HasValue());
            REQUIRE(publisher.Publish(second, std::span{&event, 1}).HasValue());
        }

        TEST_CASE("Validation announcements use the published error and retain owned speech", "[runtime_ui][accessibility][delivery]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree);
            auto publisher = MakePublisher(tree);
            ProjectionFixture fixture;
            fixture.nodes[3].state = UiAccessibilityState{UiAccessibilityStateFlag::Invalid};
            fixture.nodes[3].error = {UiAccessibilityErrorKind::Custom, {"Inventory is full", UiAccessibilityTextSource::ResolvedMessage}};
            const auto snapshot = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            auto event = Status(snapshot, 1, "Wrong message");
            event.kind = UiAccessibilityAnnouncementKind::Validation;
            RequireError(publisher.Publish(snapshot, std::span{&event, 1}), UiErrors::AccessibilitySchemaInvalid);
            event.text = fixture.nodes[3].error.message;
            REQUIRE(publisher.Publish(snapshot, std::span{&event, 1}).HasValue());
            const auto record = publisher.Announcements().front();
            REQUIRE(record.kind == UiAccessibilityAnnouncementKind::Validation);
            REQUIRE(publisher.AnnouncementText(record.cursor) == "Inventory is full");
            publisher.Retire();
            REQUIRE(publisher.Announcements().front().state == UiAccessibilityAnnouncementState::Cancelled);
            REQUIRE(publisher.Announcements().front().cancellation == UiAccessibilityAnnouncementCancellation::Retired);
            REQUIRE(publisher.AnnouncementText(record.cursor).empty());
            REQUIRE(publisher.Acknowledge(record.cursor).HasValue());
            REQUIRE(publisher.Announcements().empty());
        }

        TEST_CASE("Modal reactivation cancels old speech by generation while retained snapshots stay immutable",
                  "[runtime_ui][accessibility][delivery]") {
            auto tree = MakeTree();
            auto graph = Focus(tree);
            auto extractor = MakeExtractor(tree, 3);
            auto publisher = MakePublisher(tree);
            ProjectionFixture fixture;
            const UiFocusModalDescriptor modal{tree.Find(Stable<UiElementId>(5)).Value(), Stable<UiElementId>(5), Stable<UiElementId>(4)};
            const auto opened = graph.PushModal(modal).Value();
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View(), graph).Value();
            auto event = Status(first, 1);
            REQUIRE(publisher.Publish(first, std::span{&event, 1}).HasValue());
            REQUIRE(graph.PopModal(opened.modal).HasValue());
            const auto reopened = graph.PushModal(modal).Value();
            REQUIRE(reopened.modal != opened.modal);
            const auto second = extractor.Extract(tree, Descriptor(tree, 2), fixture.View(), graph).Value();
            event.id = {2};
            accessibilityAllocations.store(0);
            const auto update = publisher.Publish(second, std::span{&event, 1});
            const auto allocations = accessibilityAllocations.load();
            REQUIRE(update.HasValue());
            REQUIRE(allocations == 0);
            REQUIRE(publisher.Announcements()[0].cancellation == UiAccessibilityAnnouncementCancellation::OwnerChanged);
            REQUIRE(publisher.AnnouncementText(publisher.Announcements()[0].cursor).empty());
            REQUIRE(publisher.Announcements()[1].state == UiAccessibilityAnnouncementState::Pending);
            REQUIRE(first.FocusState()->activeModal == opened.modal);
            REQUIRE(second.FocusState()->activeModal == reopened.modal);
            const auto last = publisher.Announcements().back().cursor;
            accessibilityAllocations.store(0);
            publisher.Retire();
            const auto ack = publisher.Acknowledge(last);
            const auto retiredAllocations = accessibilityAllocations.load();
            REQUIRE(ack.HasValue());
            REQUIRE(retiredAllocations == 0);
        }

        TEST_CASE("Reload and inactive nodes terminate accepted occurrences with explicit cancellation",
                  "[runtime_ui][accessibility][delivery]") {
            auto slots = UiElementSlotAllocator::Create(Owner()).Value();
            auto tree = MakeTree(Owner(), 3, &slots);
            auto extractor = MakeExtractor(tree);
            auto publisher = MakePublisher(tree);
            ProjectionFixture fixture;
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            auto event = Status(first, 1);
            REQUIRE(publisher.Publish(first, std::span{&event, 1}).HasValue());
            SECTION("new document and recycled element handles") {
                auto replacement = MakeTree(Owner(), 4, &slots);
                const auto next = extractor.Extract(replacement, Descriptor(replacement, 2), fixture.View()).Value();
                REQUIRE(publisher.Publish(next).HasValue());
                REQUIRE(publisher.Announcements()[0].cancellation == UiAccessibilityAnnouncementCancellation::OwnerChanged);
            }
            SECTION("suspended control") {
                fixture.nodes[3].exposure = UiAccessibilityExposure::Suspended;
                const auto next = extractor.Extract(tree, Descriptor(tree, 2), fixture.View()).Value();
                REQUIRE(publisher.Publish(next).HasValue());
                REQUIRE(publisher.Announcements()[0].cancellation == UiAccessibilityAnnouncementCancellation::NodeUnavailable);
            }
            REQUIRE(publisher.Announcements().size() == 1);
            REQUIRE(publisher.Announcements()[0].state == UiAccessibilityAnnouncementState::Cancelled);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
