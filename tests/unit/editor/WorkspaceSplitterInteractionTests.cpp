#include "layout/WorkspaceSplitterInteraction.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo::Editor;

    constexpr WorkspaceSplitterRegion kLeftRegion{
        .id = WorkspaceSplitterId::Left,
        .axis = WorkspaceSplitterAxis::Horizontal,
        .minX = 276.0F,
        .minY = 0.0F,
        .maxX = 284.0F,
        .maxY = 600.0F,
    };

    TEST_CASE("Captures Drag Without Depending On An Im Gui Window", "[unit][editor]") {
        Horo::Input::InputRouter router;
        auto context = router.PushContext(Horo::Input::InputContextId{"workspace"}, Horo::Input::InputContextKind::EditorWorkspace);
        WorkspaceSplitterInteraction interaction;
        const std::array regions{kLeftRegion};

        const auto pressed = interaction.Update(regions,
                                                WorkspaceSplitterPointerInput{.x = 280.0F,
                                                                              .y = 240.0F,
                                                                              .deltaX = 2.0F,
                                                                              .deltaY = 0.0F,
                                                                              .primaryClicked = true,
                                                                              .primaryDown = true},
                                                router, context);
        REQUIRE((pressed.active == WorkspaceSplitterId::Left));
        REQUIRE((pressed.axis == WorkspaceSplitterAxis::Horizontal));
        REQUIRE((pressed.delta == 2.0F));
        REQUIRE((interaction.OwnsPrimaryPointer()));

        const auto draggedOutside = interaction.Update(regions,
                                                       WorkspaceSplitterPointerInput{.x = 340.0F,
                                                                                     .y = 240.0F,
                                                                                     .deltaX = 12.0F,
                                                                                     .deltaY = 0.0F,
                                                                                     .primaryClicked = false,
                                                                                     .primaryDown = true},
                                                       router, context);
        REQUIRE((draggedOutside.active == WorkspaceSplitterId::Left));
        REQUIRE((draggedOutside.axis == WorkspaceSplitterAxis::Horizontal));
        REQUIRE((draggedOutside.delta == 12.0F));

        const auto released = interaction.Update(regions,
                                                 WorkspaceSplitterPointerInput{.x = 340.0F,
                                                                               .y = 240.0F,
                                                                               .deltaX = 0.0F,
                                                                               .deltaY = 0.0F,
                                                                               .primaryClicked = false,
                                                                               .primaryDown = false},
                                                 router, context);
        REQUIRE((released.active == WorkspaceSplitterId::None));
        REQUIRE((released.delta == 0.0F));
        REQUIRE((!interaction.OwnsPrimaryPointer()));
    }

    TEST_CASE("Reports Hover Without Capturing", "[unit][editor]") {
        Horo::Input::InputRouter router;
        auto context = router.PushContext(Horo::Input::InputContextId{"workspace"}, Horo::Input::InputContextKind::EditorWorkspace);
        WorkspaceSplitterInteraction interaction;
        const std::array regions{kLeftRegion};

        const auto hovered = interaction.Update(regions,
                                                WorkspaceSplitterPointerInput{.x = 280.0F,
                                                                              .y = 240.0F,
                                                                              .deltaX = 0.0F,
                                                                              .deltaY = 0.0F,
                                                                              .primaryClicked = false,
                                                                              .primaryDown = false},
                                                router, context);
        REQUIRE((hovered.hovered == WorkspaceSplitterId::Left));
        REQUIRE((hovered.active == WorkspaceSplitterId::None));
        REQUIRE((hovered.axis == WorkspaceSplitterAxis::Horizontal));
        REQUIRE((hovered.delta == 0.0F));
    }

    TEST_CASE("Bottom Resize Owns Pointer Outside The Overlapping Seam", "[unit][editor]") {
        Horo::Input::InputRouter router;
        auto context = router.PushContext(Horo::Input::InputContextId{"workspace"}, Horo::Input::InputContextKind::EditorWorkspace);
        WorkspaceSplitterInteraction interaction;
        const std::array regions{WorkspaceSplitterRegion{
            .id = WorkspaceSplitterId::Bottom,
            .axis = WorkspaceSplitterAxis::Vertical,
            .minX = 36.0F,
            .minY = 496.0F,
            .maxX = 1244.0F,
            .maxY = 504.0F,
        }};

        const auto pressed = interaction.Update(regions,
                                                WorkspaceSplitterPointerInput{.x = 640.0F,
                                                                              .y = 502.0F,
                                                                              .deltaX = 0.0F,
                                                                              .deltaY = -3.0F,
                                                                              .primaryClicked = true,
                                                                              .primaryDown = true},
                                                router, context);
        REQUIRE((pressed.active == WorkspaceSplitterId::Bottom));
        REQUIRE((pressed.axis == WorkspaceSplitterAxis::Vertical));
        REQUIRE((pressed.delta == -3.0F));
        REQUIRE((interaction.OwnsPrimaryPointer()));

        const auto draggedIntoPanel = interaction.Update(regions,
                                                         WorkspaceSplitterPointerInput{.x = 640.0F,
                                                                                       .y = 470.0F,
                                                                                       .deltaX = 0.0F,
                                                                                       .deltaY = -12.0F,
                                                                                       .primaryClicked = false,
                                                                                       .primaryDown = true},
                                                         router, context);
        REQUIRE((draggedIntoPanel.active == WorkspaceSplitterId::Bottom));
        REQUIRE((draggedIntoPanel.delta == -12.0F));
        REQUIRE((interaction.OwnsPrimaryPointer()));
    }
}  // namespace
