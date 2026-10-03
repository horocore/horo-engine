#include "UiAccessibilityTestUtils.h"

#include <string>

namespace Horo::Runtime::Ui {
    namespace {
        TEST_CASE("Runtime UI reading projection follows retained order and exposure", "[runtime_ui][accessibility][projection]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree);
            ProjectionFixture fixture;
            std::swap(fixture.nodes[0], fixture.nodes[3]);
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View());
            REQUIRE(first.HasValue());
            REQUIRE(first.Value().Nodes()[0].elementId == Stable<UiElementId>(1));
            REQUIRE(first.Value().Nodes()[3].elementId == Stable<UiElementId>(4));
            REQUIRE(first.Value().Nodes()[3].parent == first.Value().Nodes()[0].id);

            SECTION("hidden subtree cannot leak visible children") {
                fixture.nodes[3].exposure = UiAccessibilityExposure::Hidden;
                const auto hidden = extractor.Extract(tree, Descriptor(tree, 2), fixture.View());
                REQUIRE(hidden.HasValue());
                REQUIRE(hidden.Value().Nodes().empty());
            }
            SECTION("hidden relation target fails transactionally") {
                fixture.nodes[1].exposure = UiAccessibilityExposure::Hidden;
                RequireError(extractor.Extract(tree, Descriptor(tree, 2), fixture.View()), UiErrors::AccessibilityRelationInvalid);
                fixture.nodes[1].exposure = UiAccessibilityExposure::Visible;
                REQUIRE(extractor.Extract(tree, Descriptor(tree, 2), fixture.View()).HasValue());
            }
            SECTION("inactive ancestor rejects descendant focus") {
                fixture.nodes[3].exposure = UiAccessibilityExposure::Covered;
                fixture.nodes[2].state = UiAccessibilityStateFlag::Focusable | UiAccessibilityStateFlag::Focused;
                RequireError(extractor.Extract(tree, Descriptor(tree, 2), fixture.View()), UiErrors::AccessibilityStateInvalid);
            }
            SECTION("offscreen and disabled controls remain readable") {
                fixture.nodes[3].exposure = UiAccessibilityExposure::Offscreen;
                fixture.nodes[2].state = UiAccessibilityStateFlag::Disabled;
                const auto offscreen = extractor.Extract(tree, Descriptor(tree, 2), fixture.View());
                REQUIRE(offscreen.HasValue());
                REQUIRE(offscreen.Value().Nodes()[2].exposure == UiAccessibilityExposure::Offscreen);
                REQUIRE(offscreen.Value().Nodes()[2].state.Has(UiAccessibilityStateFlag::Disabled));
            }
        }

        TEST_CASE("Runtime UI semantic changes compare owned properties and authoritative focus", "[runtime_ui][accessibility][changes]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree, 3);
            auto publisher = MakePublisher(tree);
            ProjectionFixture fixture;
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            REQUIRE(publisher.Publish(first).HasValue());
            REQUIRE(publisher.Changes().size() == 4);
            REQUIRE(!publisher.PreviousRevision().IsValid());
            REQUIRE(publisher.Changes()[0].kind == UiAccessibilityChangeKind::Inserted);
            REQUIRE(publisher.Changes()[2].previous == first.Nodes()[1].id);

            fixture.nodes[0].name = {"Longer menu name", UiAccessibilityTextSource::ResolvedMessage};
            fixture.nodes[2].value.number = 0.75;
            fixture.nodes[2].range.current = 0.75;
            fixture.nodes[2].state = UiAccessibilityStateFlag::Focusable | UiAccessibilityStateFlag::Focused;
            fixture.sliderActions[0].name = {"Raise volume", UiAccessibilityTextSource::UserContent};
            const auto second = extractor.Extract(tree, Descriptor(tree, 2), fixture.View()).Value();
            REQUIRE(publisher.Publish(second).HasValue());
            REQUIRE(publisher.PreviousRevision() == first.Descriptor().semanticRevision);
            REQUIRE(publisher.Changes().size() == 3);
            REQUIRE(publisher.Changes()[0].properties == static_cast<std::uint32_t>(UiAccessibilityProperty::Name));
            REQUIRE(publisher.Changes()[1].node == second.Nodes()[2].id);
            REQUIRE(publisher.Changes()[1].properties == (static_cast<std::uint32_t>(UiAccessibilityProperty::Value) |
                                                          static_cast<std::uint32_t>(UiAccessibilityProperty::Range) |
                                                          static_cast<std::uint32_t>(UiAccessibilityProperty::State) |
                                                          static_cast<std::uint32_t>(UiAccessibilityProperty::Actions)));
            REQUIRE(publisher.Changes()[2].kind == UiAccessibilityChangeKind::Focus);
            REQUIRE(publisher.Changes()[2].node == second.Nodes()[2].id);

            const auto third = extractor.Extract(tree, Descriptor(tree, 3), fixture.View()).Value();
            REQUIRE(publisher.Publish(third).HasValue());
            REQUIRE(publisher.Changes().empty());
            RequireError(publisher.Publish(third), UiErrors::AccessibilitySnapshotSourceStale);
            REQUIRE(publisher.Revision() == third.Descriptor().semanticRevision);
        }

        TEST_CASE("Runtime UI structural changes distinguish reorder and recycled generations", "[runtime_ui][accessibility][changes]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree, 3);
            auto publisher = MakePublisher(tree);
            ProjectionFixture fixture;
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            REQUIRE(publisher.Publish(first).HasValue());
            auto commands =
                UiStructuralCommandBuffer::Create(tree.Instance(), tree.Canvas(), tree.SourceDocumentRevision(), tree.Revision(), 3)
                    .Value();
            REQUIRE(commands.Add(UiReparentElementCommand{tree.Find(Stable<UiElementId>(3)).Value(), tree.Root().Value().handle, 0})
                        .HasValue());
            REQUIRE(tree.CommitDeferred(commands, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
            const auto reordered = extractor.Extract(tree, Descriptor(tree, 2), fixture.View()).Value();
            REQUIRE(publisher.Publish(reordered).HasValue());
            REQUIRE(publisher.Changes().size() == 3);
            for (const auto &change : publisher.Changes())
                REQUIRE(change.kind == UiAccessibilityChangeKind::Structure);

            commands = UiStructuralCommandBuffer::Create(tree.Instance(), tree.Canvas(), tree.SourceDocumentRevision(), tree.Revision(), 3)
                           .Value();
            const auto old = tree.Find(Stable<UiElementId>(4)).Value();
            REQUIRE(commands.Add(UiRemoveElementCommand{old}).HasValue());
            REQUIRE(commands.Add(UiInsertElementCommand{Stable<UiElementId>(4), tree.Find(Stable<UiElementId>(5)).Value(), 0}).HasValue());
            REQUIRE(tree.CommitDeferred(commands, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
            const auto recycled = extractor.Extract(tree, Descriptor(tree, 3), fixture.View()).Value();
            REQUIRE(publisher.Publish(recycled).HasValue());
            REQUIRE(publisher.Changes().size() == 2);
            REQUIRE(publisher.Changes()[0].kind == UiAccessibilityChangeKind::Removed);
            REQUIRE(publisher.Changes()[1].kind == UiAccessibilityChangeKind::Inserted);
            REQUIRE(publisher.Changes()[0].node != publisher.Changes()[1].node);
            REQUIRE(recycled.Get(first.Nodes()[3].id).HasError());
        }

        TEST_CASE("Runtime UI announcements own text and deduplicate within explicit revisions",
                  "[runtime_ui][accessibility][announcements]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree, 4);
            auto publisher = MakePublisher(tree, {16, 2, 64, 2});
            ProjectionFixture fixture;
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            std::string source = "Inventory opened";
            std::array announcement{UiAccessibilityAnnouncementInput{{1},
                                                                     first.Nodes()[3].id,
                                                                     UiAccessibilityAnnouncementPolicy::Assertive,
                                                                     {source, UiAccessibilityTextSource::UserContent}}};
            REQUIRE(publisher.Publish(first, announcement).HasValue());
            REQUIRE(publisher.Changes().back().kind == UiAccessibilityChangeKind::Announcement);
            source[0] = 'X';
            REQUIRE(publisher.Text(publisher.Changes().back().text) == "Inventory opened");
            const auto second = extractor.Extract(tree, Descriptor(tree, 3), fixture.View()).Value();
            REQUIRE(publisher.Publish(second, announcement).HasValue());
            REQUIRE(publisher.Changes().empty());
            REQUIRE(publisher.Acknowledge(publisher.Announcements().front().cursor).HasValue());
            const auto third = extractor.Extract(tree, Descriptor(tree, 4), fixture.View()).Value();
            REQUIRE(publisher.Publish(third, announcement).HasValue());
            REQUIRE(publisher.Changes().size() == 1);
            REQUIRE(publisher.Changes()[0].policy == UiAccessibilityAnnouncementPolicy::Assertive);

            const auto fourth = extractor.Extract(tree, Descriptor(tree, 5), fixture.View()).Value();
            announcement[0].id = {2};
            announcement[0].policy = UiAccessibilityAnnouncementPolicy::Off;
            REQUIRE(publisher.Publish(fourth, announcement).HasValue());
            REQUIRE(publisher.Changes().empty());
        }

        TEST_CASE("Runtime UI delta overflow requires resync and never replays announcements", "[runtime_ui][accessibility][overflow]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree);
            auto publisher = MakePublisher(tree, {1, 2, 64, 4});
            ProjectionFixture fixture;
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            const std::array announcement{
                UiAccessibilityAnnouncementInput{{1}, first.Nodes()[0].id, UiAccessibilityAnnouncementPolicy::Polite, {"Ready", {}}}};
            REQUIRE(publisher.Publish(first, announcement).HasValue());
            REQUIRE(publisher.Status() == UiAccessibilityChangeStatus::Resynchronize);
            REQUIRE(publisher.Changes().size() == 1);
            REQUIRE(publisher.Changes()[0].kind == UiAccessibilityChangeKind::Resynchronize);
            REQUIRE(publisher.Text(publisher.Changes()[0].text).empty());
            const auto second = extractor.Extract(tree, Descriptor(tree, 2), fixture.View()).Value();
            REQUIRE(publisher.Publish(second, announcement).HasValue());
            REQUIRE(publisher.Status() == UiAccessibilityChangeStatus::Complete);
            REQUIRE(publisher.Changes().empty());
            publisher.Retire();
            REQUIRE(publisher.IsRetired());
            REQUIRE(publisher.Status() == UiAccessibilityChangeStatus::Resynchronize);
            REQUIRE(publisher.Changes().size() == 1);
        }

        TEST_CASE("Runtime UI rejected announcements preserve baseline output and dedup history",
                  "[runtime_ui][accessibility][validation]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree);
            auto publisher = MakePublisher(tree, {16, 1, 8, 32});
            ProjectionFixture fixture;
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            const std::array initial{
                UiAccessibilityAnnouncementInput{{1}, first.Nodes()[0].id, UiAccessibilityAnnouncementPolicy::Polite, {"Ready", {}}}};
            REQUIRE(publisher.Publish(first, initial).HasValue());
            const auto second = extractor.Extract(tree, Descriptor(tree, 2), fixture.View()).Value();
            auto invalid = initial;
            SECTION("history capacity cannot evict an unexpired identity") {
                invalid[0].id = {2};
                RequireError(publisher.Publish(second, invalid), UiErrors::CapacityExceeded);
            }
            SECTION("text capacity is checked before copying") {
                invalid[0].text.text = "Oversized announcement";
                RequireError(publisher.Publish(second, invalid), UiErrors::CapacityExceeded);
            }
            SECTION("malformed identity") {
                invalid[0].id = {};
                RequireError(publisher.Publish(second, invalid), UiErrors::AccessibilitySchemaInvalid);
            }
            SECTION("stale node") {
                invalid[0].node.generation += 1;
                RequireError(publisher.Publish(second, invalid), UiErrors::AccessibilitySnapshotSourceStale);
            }
            SECTION("invalid UTF8") {
                invalid[0].text.text = "\xFF";
                RequireError(publisher.Publish(second, invalid), UiErrors::AccessibilitySchemaInvalid);
            }
            REQUIRE(publisher.Revision() == first.Descriptor().semanticRevision);
            REQUIRE(publisher.Text(publisher.Changes().back().text) == "Ready");
            REQUIRE(publisher.Publish(second, initial).HasValue());
            REQUIRE(publisher.Changes().empty());
        }

        TEST_CASE("Runtime UI retirement closes admission and releases its snapshot lease", "[runtime_ui][accessibility][lifecycle]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree);
            auto publisher = MakePublisher(tree);
            ProjectionFixture fixture;
            fixture.nodes[2].state = UiAccessibilityStateFlag::Focusable | UiAccessibilityStateFlag::Focused;
            std::optional<UiAccessibilitySnapshot> snapshot{extractor.Extract(tree, Descriptor(tree), fixture.View()).Value()};
            REQUIRE(publisher.Publish(*snapshot).HasValue());
            extractor.Close();
            tree.Shutdown();
            snapshot.reset();
            REQUIRE(!extractor.IsDrained());
            publisher.Retire();
            REQUIRE(extractor.IsDrained());
            REQUIRE(publisher.Status() == UiAccessibilityChangeStatus::Retired);
            REQUIRE(publisher.Changes().size() == 5);
            REQUIRE(publisher.Changes()[0].node.slot > publisher.Changes()[3].node.slot);
            REQUIRE(publisher.Changes().back().kind == UiAccessibilityChangeKind::Focus);
            REQUIRE(!publisher.Changes().back().node.IsValid());
            publisher.Retire();
            REQUIRE(publisher.Changes().size() == 5);
            auto moved = std::move(publisher);
            REQUIRE(publisher.IsRetired());
            REQUIRE(publisher.Changes().empty());
            REQUIRE(moved.Changes().size() == 5);
        }

        TEST_CASE("Runtime UI semantic change publication allocates no frame-hot storage", "[runtime_ui][accessibility][performance]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree);
            auto publisher = MakePublisher(tree);
            ProjectionFixture fixture;
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            const std::array announcement{
                UiAccessibilityAnnouncementInput{{1}, first.Nodes()[0].id, UiAccessibilityAnnouncementPolicy::Polite, {"Ready", {}}}};
            accessibilityAllocations.store(0);
            const auto result = publisher.Publish(first, announcement);
            const auto allocations = accessibilityAllocations.load();
            REQUIRE(result.HasValue());
            REQUIRE(allocations == 0);
            fixture.nodes[0].description = {"Accessible description", {}};
            const auto second = extractor.Extract(tree, Descriptor(tree, 2), fixture.View()).Value();
            accessibilityAllocations.store(0);
            const auto update = publisher.Publish(second);
            const auto updateAllocations = accessibilityAllocations.load();
            REQUIRE(update.HasValue());
            REQUIRE(updateAllocations == 0);
        }

        TEST_CASE("Runtime UI property changes include all semantic groups", "[runtime_ui][accessibility][changes]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree);
            auto publisher = MakePublisher(tree);
            ProjectionFixture fixture;
            auto &node = fixture.nodes[3];
            node.role = UiAccessibilityRole::ListItem;
            node.hasSelection = true;
            node.selection = {UiAccessibilitySelectionMode::Single, false, 0, 2};
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            REQUIRE(publisher.Publish(first).HasValue());
            node.role = UiAccessibilityRole::TabItem;
            node.source = UiAccessibilityControlSource::Core;
            node.contributor = {};
            node.description = {"Tab description", {}};
            node.selection.selected = true;
            node.state = UiAccessibilityStateFlag::Selected | UiAccessibilityStateFlag::Invalid;
            node.error = {UiAccessibilityErrorKind::Custom, {"Unavailable", {}}};
            node.exposure = UiAccessibilityExposure::Offscreen;
            node.bounds.origin.x = 12;
            const std::array relations{UiAccessibilityRelationInput{UiAccessibilityRelationKind::Controls, Stable<UiElementId>(3)}};
            node.relations = relations;
            const auto second = extractor.Extract(tree, Descriptor(tree, 2), fixture.View()).Value();
            REQUIRE(publisher.Publish(second).HasValue());
            REQUIRE(publisher.Changes().size() == 1);
            REQUIRE(publisher.Changes()[0].properties == (static_cast<std::uint32_t>(UiAccessibilityProperty::Role) |
                                                          static_cast<std::uint32_t>(UiAccessibilityProperty::Source) |
                                                          static_cast<std::uint32_t>(UiAccessibilityProperty::Description) |
                                                          static_cast<std::uint32_t>(UiAccessibilityProperty::Selection) |
                                                          static_cast<std::uint32_t>(UiAccessibilityProperty::State) |
                                                          static_cast<std::uint32_t>(UiAccessibilityProperty::Error) |
                                                          static_cast<std::uint32_t>(UiAccessibilityProperty::Exposure) |
                                                          static_cast<std::uint32_t>(UiAccessibilityProperty::Bounds) |
                                                          static_cast<std::uint32_t>(UiAccessibilityProperty::Relations)));
        }

        TEST_CASE("Runtime UI typed values compare active alternatives", "[runtime_ui][accessibility][changes]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree);
            auto publisher = MakePublisher(tree);
            ProjectionFixture fixture;
            auto &node = fixture.nodes[3];
            node.actions = {};
            SECTION("boolean") {
                node.role = UiAccessibilityRole::Checkbox;
                node.value = {UiAccessibilityValueKind::Boolean, false, 0, 0.0, {}};
            }
            SECTION("integer") {
                node.role = UiAccessibilityRole::Progress;
                node.value = {UiAccessibilityValueKind::Integer, false, 2, 0.0, {}};
            }
            SECTION("text") {
                node.role = UiAccessibilityRole::TextField;
                node.value = {UiAccessibilityValueKind::Text, false, 0, 0.0, {"old", {}}};
            }
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            REQUIRE(publisher.Publish(first).HasValue());
            node.value.boolean = true;
            node.value.integer = 4;
            node.value.text = {"new", UiAccessibilityTextSource::UserContent};
            const auto second = extractor.Extract(tree, Descriptor(tree, 2), fixture.View()).Value();
            REQUIRE(publisher.Publish(second).HasValue());
            REQUIRE(publisher.Changes().size() == 1);
            REQUIRE(publisher.Changes()[0].properties == static_cast<std::uint32_t>(UiAccessibilityProperty::Value));
        }

        TEST_CASE("Runtime UI source replacement is fenced while newer documents may reset tree revisions",
                  "[runtime_ui][accessibility][lifecycle]") {
            auto ownerSlots = UiElementSlotAllocator::Create(Owner()).Value();
            auto tree = MakeTree(Owner(), 3, &ownerSlots);
            auto extractor = MakeExtractor(tree, 3);
            auto publisher = MakePublisher(tree);
            ProjectionFixture fixture;
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            REQUIRE(publisher.Publish(first).HasValue());
            auto other = MakeTree(Owner(99));
            const auto foreign = MakeExtractor(other).Extract(other, Descriptor(other, 2), fixture.View()).Value();
            RequireError(publisher.Publish(foreign), UiErrors::AccessibilitySnapshotSourceStale);
            auto olderInteraction = Descriptor(tree, 2);
            olderInteraction.interactionRevision = UiInteractionRevision::Create(1).Value();
            auto stale = extractor.Extract(tree, olderInteraction, fixture.View()).Value();
            RequireError(publisher.Publish(stale), UiErrors::AccessibilitySnapshotSourceStale);
            auto live = std::move(stale);
            REQUIRE(!stale.IsValid());
            RequireError(publisher.Publish(stale), UiErrors::AccessibilityLifecycleUnavailable);
            REQUIRE(publisher.Revision() == first.Descriptor().semanticRevision);

            auto replacement = MakeTree(Owner(), 4, &ownerSlots);
            const auto reloaded = extractor.Extract(replacement, Descriptor(replacement, 3), fixture.View()).Value();
            REQUIRE(publisher.Publish(reloaded).HasValue());
            publisher.Retire();
            RequireError(publisher.Publish(reloaded), UiErrors::AccessibilityLifecycleUnavailable);
        }

        TEST_CASE("Runtime UI hidden removal is distinct from inactive exposure and announcements",
                  "[runtime_ui][accessibility][projection]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree, 4);
            auto publisher = MakePublisher(tree);
            ProjectionFixture fixture;
            const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            REQUIRE(publisher.Publish(first).HasValue());
            fixture.nodes[0].exposure = UiAccessibilityExposure::Suspended;
            fixture.nodes[3].exposure = UiAccessibilityExposure::Hidden;
            const auto inactive = extractor.Extract(tree, Descriptor(tree, 2), fixture.View()).Value();
            REQUIRE(inactive.Nodes().size() == 3);
            REQUIRE(inactive.Nodes()[2].exposure == UiAccessibilityExposure::Suspended);
            REQUIRE(publisher.Publish(inactive).HasValue());
            REQUIRE(publisher.Changes()[0].kind == UiAccessibilityChangeKind::Removed);
            const std::array announcement{
                UiAccessibilityAnnouncementInput{{1}, inactive.Nodes()[0].id, UiAccessibilityAnnouncementPolicy::Polite, {"Ready", {}}}};
            fixture.nodes[0].exposure = UiAccessibilityExposure::Suppressed;
            const auto suppressed = extractor.Extract(tree, Descriptor(tree, 3), fixture.View()).Value();
            RequireError(publisher.Publish(suppressed, announcement), UiErrors::AccessibilityActionRejected);
            REQUIRE(publisher.Revision() == inactive.Descriptor().semanticRevision);
        }

        TEST_CASE("Runtime UI change budgets and duplicate announcements fail explicitly", "[runtime_ui][accessibility][validation]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree);
            ProjectionFixture fixture;
            const auto snapshot = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
            REQUIRE(!UiAccessibilityChangeLimits{0, 0, 0, 0}.IsValid());
            REQUIRE(!UiAccessibilityChangeLimits{MaximumUiAccessibilityChanges + 1, 0, 0, 0}.IsValid());
            REQUIRE(!UiAccessibilityChangeLimits{1, MaximumUiAccessibilityAnnouncements + 1, 0, 0}.IsValid());
            auto publisher = MakePublisher(tree);
            const UiAccessibilityAnnouncementInput event{{1},
                                                         snapshot.Nodes()[0].id,
                                                         UiAccessibilityAnnouncementPolicy::Polite,
                                                         {"Ready", {}}};
            const std::array duplicate{event, event};
            RequireError(publisher.Publish(snapshot, duplicate), UiErrors::AccessibilitySchemaInvalid);
            REQUIRE(!publisher.Revision().IsValid());
            REQUIRE(publisher.Publish(snapshot, std::span{&event, 1}).HasValue());
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
