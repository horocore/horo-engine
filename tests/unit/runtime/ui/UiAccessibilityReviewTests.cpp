#include "Horo/Runtime/Ui/UiAccessibilityReview.h"
#include "UiAccessibilityTestUtils.h"

namespace Horo::Runtime::Ui {
    TEST_CASE("Accessibility QA log rejects moved snapshot leases and omits oversized text without UTF-8 truncation",
              "[runtime_ui][accessibility][review]") {
        auto tree = MakeTree();
        auto extractor = MakeExtractor(tree);
        auto publisher = MakePublisher(tree);
        ProjectionFixture fixture;
        const std::string oversized(300, 'x');
        fixture.nodes[0].name = {oversized, UiAccessibilityTextSource::UserContent};
        auto snapshot = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
        REQUIRE(publisher.Publish(snapshot).HasValue());
        auto retained = std::move(snapshot);
        UiAccessibilityReviewLog log;
        log.SetEnabled(true, true);
        REQUIRE(log.Record(snapshot, publisher).HasError());
        REQUIRE(log.Size() == 0);
        REQUIRE(log.Record(retained, publisher).HasValue());
        REQUIRE(log.Entry(0)->textOmitted);
        REQUIRE(log.Entry(0)->Name().empty());
    }

    TEST_CASE("Accessibility QA log is opt-in, owns copied content and never acknowledges delivery",
              "[runtime_ui][accessibility][review]") {
        auto tree = MakeTree();
        auto extractor = MakeExtractor(tree, 3);
        auto publisher = MakePublisher(tree);
        ProjectionFixture fixture;
        const auto first = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
        REQUIRE(publisher.Publish(first).HasValue());
        UiAccessibilityReviewLog log;
        REQUIRE(log.Record(first, publisher).Value() == 0);
        REQUIRE(log.Size() == 0);
        log.SetEnabled(true);
        REQUIRE(log.Record(first, publisher).Value() == 4);
        REQUIRE(log.Entry(0)->Name().empty());
        log.SetEnabled(true, true);
        REQUIRE(log.Size() == 0);
        REQUIRE(log.Record(first, publisher).Value() == 4);
        REQUIRE(log.Entry(0)->Name() == "Main menu");
        fixture.nodes[0].name = {"Changed menu", UiAccessibilityTextSource::ResolvedMessage};
        fixture.nodes[2].value.number = 0.75;
        fixture.nodes[2].range.current = 0.75;
        fixture.nodes[2].state = UiAccessibilityStateFlag::Focusable | UiAccessibilityStateFlag::Focused;
        const auto second = extractor.Extract(tree, Descriptor(tree, 2), fixture.View()).Value();
        REQUIRE(publisher.Publish(second).HasValue());
        REQUIRE(log.Record(first, publisher).HasError());
        REQUIRE(log.Size() == 4);
        REQUIRE(log.Record(second, publisher).Value() == 3);
        REQUIRE(log.Entry(0)->Name() == "Main menu");
        REQUIRE(log.Entry(5)->number == 0.75);
        REQUIRE(log.Entry(6)->kind == UiAccessibilityChangeKind::Focus);
        REQUIRE(publisher.Announcements().empty());
        log.SetEnabled(false);
        REQUIRE(log.Size() == 0);
        REQUIRE(log.Entry(0) == nullptr);
    }

    TEST_CASE("Accessibility QA journal overwrites only diagnostics within its fixed capacity", "[runtime_ui][accessibility][review]") {
        auto tree = MakeTree();
        auto extractor = MakeExtractor(tree, 3);
        auto publisher = MakePublisher(tree);
        ProjectionFixture fixture;
        UiAccessibilityReviewLog log;
        log.SetEnabled(true, true);
        std::size_t observed = 0;
        for (std::uint64_t revision = 1; revision <= 100; ++revision) {
            fixture.nodes[0].name = {revision % 2 ? "Odd menu" : "Even menu", UiAccessibilityTextSource::ResolvedMessage};
            const auto snapshot = extractor.Extract(tree, Descriptor(tree, revision), fixture.View()).Value();
            REQUIRE(publisher.Publish(snapshot).HasValue());
            observed += log.Record(snapshot, publisher).Value();
        }
        REQUIRE(log.Size() == UiAccessibilityReviewLog::Capacity);
        REQUIRE(log.Overwritten() == observed - UiAccessibilityReviewLog::Capacity);
        REQUIRE(log.Entry(log.Size() - 1)->Name() == "Even menu");
        REQUIRE(log.Entry(log.Size()) == nullptr);
        log.Clear();
        REQUIRE(log.Size() == 0);
        REQUIRE(log.Overwritten() == 0);
    }
}  // namespace Horo::Runtime::Ui
