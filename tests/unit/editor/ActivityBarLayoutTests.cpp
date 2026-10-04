#include "Horo/Editor/ActivityBarLayout.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

namespace {
    using namespace Horo::Editor;

    TEST_CASE("Moves An Item Into An Insertion Slot", "[unit][editor]") {
        ActivityBarLayout layout;
        static_cast<void>(layout.Insert("horo.hierarchy", ActivityBarSlot{ActivityBarRail::Left, 0, 0}));
        static_cast<void>(layout.Insert("horo.inspector", ActivityBarSlot{ActivityBarRail::Left, 0, 1}));

        const auto result = layout.Move("horo.inspector", ActivityBarSlot{ActivityBarRail::Left, 1, 0});
        REQUIRE((result.Succeeded()));
        REQUIRE((layout.FindSlot("horo.inspector").value() == (ActivityBarSlot{ActivityBarRail::Left, 1, 0})));
        REQUIRE((layout.FindSlot("horo.hierarchy").value() == (ActivityBarSlot{ActivityBarRail::Left, 0, 0})));
    }

    TEST_CASE("Moving Within A Group Shifts Existing Items", "[unit][editor]") {
        ActivityBarLayout layout;
        static_cast<void>(layout.Insert("one", ActivityBarSlot{ActivityBarRail::Left, 0, 0}));
        static_cast<void>(layout.Insert("two", ActivityBarSlot{ActivityBarRail::Left, 0, 1}));
        static_cast<void>(layout.Insert("three", ActivityBarSlot{ActivityBarRail::Left, 0, 2}));

        REQUIRE((layout.Move("three", ActivityBarSlot{ActivityBarRail::Left, 0, 1}).Succeeded()));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 0, 0) == "one"));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 0, 1) == "three"));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 0, 2) == "two"));
    }

    TEST_CASE("Rejects Unknown Item And Invalid Insertion Without Mutation", "[unit][editor]") {
        ActivityBarLayout layout;
        static_cast<void>(layout.Insert("one", ActivityBarSlot{ActivityBarRail::Left, 0, 0}));

        REQUIRE((!layout.Move("missing", ActivityBarSlot{ActivityBarRail::Left, 0, 0}).Succeeded()));
        REQUIRE((!layout.Move("one", ActivityBarSlot{ActivityBarRail::Left, 0, 3}).Succeeded()));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 0, 0) == "one"));
    }

    TEST_CASE("Dropping The Only Item Back Into Its Group Is A No Op", "[unit][editor]") {
        ActivityBarLayout layout;
        static_cast<void>(layout.Insert("one", ActivityBarSlot{ActivityBarRail::Left, 0, 0}));
        const auto result = layout.Move("one", ActivityBarSlot{ActivityBarRail::Left, 0, 1});
        REQUIRE((result.code == ActivityBarLayoutOperationCode::NoOp));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 0, 0) == "one"));
    }

    TEST_CASE("Moving Across Groups Compacts The Source And Inserts At The Target Index", "[unit][editor]") {
        ActivityBarLayout layout;
        static_cast<void>(layout.Insert("source-0", ActivityBarSlot{ActivityBarRail::Left, 1, 0}));
        static_cast<void>(layout.Insert("source-1", ActivityBarSlot{ActivityBarRail::Left, 1, 1}));
        static_cast<void>(layout.Insert("source-2", ActivityBarSlot{ActivityBarRail::Left, 1, 2}));
        static_cast<void>(layout.Insert("source-3", ActivityBarSlot{ActivityBarRail::Left, 1, 3}));
        static_cast<void>(layout.Insert("source-4", ActivityBarSlot{ActivityBarRail::Left, 1, 4}));
        static_cast<void>(layout.Insert("source-5", ActivityBarSlot{ActivityBarRail::Left, 1, 5}));
        static_cast<void>(layout.Insert("target-0", ActivityBarSlot{ActivityBarRail::Left, 0, 0}));
        static_cast<void>(layout.Insert("target-1", ActivityBarSlot{ActivityBarRail::Left, 0, 1}));
        static_cast<void>(layout.Insert("target-2", ActivityBarSlot{ActivityBarRail::Left, 0, 2}));
        static_cast<void>(layout.Insert("target-3", ActivityBarSlot{ActivityBarRail::Left, 0, 3}));

        const auto result = layout.Move("source-2", ActivityBarSlot{ActivityBarRail::Left, 0, 2});
        REQUIRE((result.Succeeded()));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 0, 0) == "target-0"));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 0, 1) == "target-1"));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 0, 2) == "source-2"));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 0, 3) == "target-2"));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 1, 0) == "source-0"));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 1, 1) == "source-1"));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 1, 2) == "source-3"));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 1, 3) == "source-4"));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 1, 4) == "source-5"));
    }

    TEST_CASE("Supports A Dedicated Document Top Rail", "[unit][editor]") {
        ActivityBarLayout layout;

        REQUIRE((layout.Groups(ActivityBarRail::DocumentTop).size() == 1));
        REQUIRE((layout.Insert("horo.viewport", ActivityBarSlot{ActivityBarRail::DocumentTop, 0, 0}).Succeeded()));
        REQUIRE((layout.FindSlot("horo.viewport") == ActivityBarSlot{ActivityBarRail::DocumentTop, 0, 0}));
        REQUIRE((layout.Insert("hidden", ActivityBarSlot{ActivityBarRail::DocumentTop, 1, 0}).code ==
                 ActivityBarLayoutOperationCode::InvalidGroup));
        REQUIRE((layout.Move("horo.viewport", ActivityBarSlot{ActivityBarRail::DocumentTop, 1, 0}).code ==
                 ActivityBarLayoutOperationCode::InvalidGroup));
        REQUIRE((layout.FindSlot("horo.viewport") == ActivityBarSlot{ActivityBarRail::DocumentTop, 0, 0}));
        REQUIRE((layout.ItemAt(ActivityBarRail::Left, 1, 0).empty()));
    }

    TEST_CASE("Withdrawing A Contribution Compacts Its Group And Allows Reattachment", "[unit][editor][extensions]") {
        ActivityBarLayout layout;
        REQUIRE(layout.Insert("horo.first", {ActivityBarRail::Right, 1, 0}).Succeeded());
        REQUIRE(layout.Insert("com.example.tools.drawer", {ActivityBarRail::Right, 1, 1}).Succeeded());
        REQUIRE(layout.Insert("horo.last", {ActivityBarRail::Right, 1, 2}).Succeeded());
        REQUIRE(layout.Insert("horo.bottom", {ActivityBarRail::Bottom, 0, 0}).Succeeded());

        REQUIRE(layout.Remove("com.example.tools.drawer").Succeeded());
        CHECK_FALSE(layout.FindSlot("com.example.tools.drawer").has_value());
        CHECK(layout.ItemAt(ActivityBarRail::Right, 1, 0) == "horo.first");
        CHECK(layout.ItemAt(ActivityBarRail::Right, 1, 1) == "horo.last");
        CHECK(layout.ItemAt(ActivityBarRail::Right, 1, 2).empty());
        CHECK(layout.ItemAt(ActivityBarRail::Bottom, 0, 0) == "horo.bottom");

        CHECK(layout.Remove("com.example.tools.drawer").code == ActivityBarLayoutOperationCode::UnknownItem);
        REQUIRE(layout.Insert("com.example.tools.drawer", {ActivityBarRail::Right, 1, 1}).Succeeded());
        CHECK(layout.ItemAt(ActivityBarRail::Right, 1, 1) == "com.example.tools.drawer");
        CHECK(layout.ItemAt(ActivityBarRail::Right, 1, 2) == "horo.last");
    }

    TEST_CASE("Withdrawing The Only Contribution Leaves A Reusable Empty Group", "[unit][editor][extensions]") {
        ActivityBarLayout layout;
        REQUIRE(layout.Insert("com.example.tools.drawer", {ActivityBarRail::Left, 0, 0}).Succeeded());
        CHECK(layout.Remove("").code == ActivityBarLayoutOperationCode::UnknownItem);
        CHECK(layout.ItemAt(ActivityBarRail::Left, 0, 0) == "com.example.tools.drawer");
        REQUIRE(layout.Remove("com.example.tools.drawer").Succeeded());
        CHECK(layout.Groups(ActivityBarRail::Left).front().items.empty());
        CHECK(layout.Groups(ActivityBarRail::Left).size() == ActivityBarLayout::kDefaultGroupCount);
        REQUIRE(layout.Insert("com.example.tools.replacement", {ActivityBarRail::Left, 0, 0}).Succeeded());
    }
}  // namespace
