#include "editor/modals/gameplay_behavior/GameplayBehaviorFilenameModalState.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo::Editor;

    TEST_CASE("Behavior filename modal preserves kind destination and editable base name", "[unit][editor][behavior]") {
        GameplayBehaviorFilenameModalState state(GameplayBehaviorKind::Native, "/project/assets/scripts", "PlayerBehavior");

        REQUIRE(state.Kind() == GameplayBehaviorKind::Native);
        REQUIRE(state.Destination() == "/project/assets/scripts");
        REQUIRE(state.BaseName() == "PlayerBehavior");
        REQUIRE(state.Validation().HasValue());
    }

    TEST_CASE("Behavior filename modal validates edits without mutating external state", "[unit][editor][behavior]") {
        GameplayBehaviorFilenameModalState state(GameplayBehaviorKind::Lua, "/project/assets/scripts", "PlayerBehavior");

        state.SetBaseName("bad/name");
        REQUIRE(state.Validation().HasError());
        REQUIRE(!state.Confirm().has_value());

        state.SetBaseName("PlayerBehavior");
        const auto request = state.Confirm();
        REQUIRE(request.has_value());
        REQUIRE(request->kind == GameplayBehaviorKind::Lua);
        REQUIRE(request->destination == "/project/assets/scripts");
        REQUIRE(request->baseName == "PlayerBehavior");
    }

    TEST_CASE("Behavior filename modal cancel never returns a creation request", "[unit][editor][behavior]") {
        GameplayBehaviorFilenameModalState state(GameplayBehaviorKind::Lua, "/project/assets/scripts", "PlayerBehavior");

        REQUIRE(!state.Dispatch(GameplayBehaviorFilenameModalAction::Cancel).has_value());
    }

    TEST_CASE("Behavior filename modal revalidates language and destination changes", "[unit][editor][behavior]") {
        GameplayBehaviorFilenameModalState state(GameplayBehaviorKind::Lua, "/project/Assets/Scripts", "Player Movement");
        REQUIRE(state.Validation().HasValue());

        state.SetKind(GameplayBehaviorKind::Native, "/project/source/gameplay");
        REQUIRE(state.Validation().HasError());
        state.SetBaseName("PlayerMovement");
        REQUIRE(state.Validation().HasValue());
        REQUIRE(state.Confirm()->kind == GameplayBehaviorKind::Native);

        state.SetKind(GameplayBehaviorKind::Lua, "/project/Assets/Scripts");
        state.SetDestination("/project/Assets/Scripts/Actors");
        REQUIRE(state.Confirm()->destination == "/project/Assets/Scripts/Actors");
    }

    TEST_CASE("Behavior filename modal confirm returns no request while invalid", "[unit][editor][behavior]") {
        GameplayBehaviorFilenameModalState state(GameplayBehaviorKind::Lua, "relative/scripts", "PlayerBehavior");

        REQUIRE(state.Validation().HasError());
        REQUIRE(!state.Dispatch(GameplayBehaviorFilenameModalAction::Confirm).has_value());
    }
}  // namespace
