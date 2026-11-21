#include "GuardCover.h"

#include <array>
#include <iostream>

namespace {
    using namespace Horo;
    using namespace Horo::AI;
    using namespace Horo::Gameplay;

    constexpr Runtime::SceneObjectId Guard{1};
    constexpr Runtime::SceneObjectId ExposedAnchor{2};
    constexpr Runtime::SceneObjectId ProtectedAnchor{3};

    /** @brief Creates a standalone deterministic fixture with a guard and two authored cover anchors. */
    Result<std::unique_ptr<Runtime::RuntimeScene>> CreateScene() {
        Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
        builder.Add({Guard});
        builder.Add({ExposedAnchor});
        builder.Add({ProtectedAnchor});
        const auto scene = std::move(builder).Build();
        if (scene.HasError())
            return Result<std::unique_ptr<Runtime::RuntimeScene>>::Failure(scene.ErrorValue());
        return Runtime::RuntimeScene::Create(scene.Value(), Runtime::SceneRuntimeId{1});
    }

    /** @brief Runs cover selection with explicit detached fixture facts and releases the exact receipt. */
    bool Run(const Runtime::RuntimeSceneView &scene) {
        const auto guard = scene.Find(Guard);
        const auto exposed = scene.Find(ExposedAnchor);
        const auto protectedAnchor = scene.Find(ProtectedAnchor);
        if (!guard || !exposed || !protectedAnchor)
            return false;
        const std::array points{TacticalCoverPoint{TacticalCoverId::Create(1).Value(),
                                                   *exposed,
                                                   Math::WorldCoordinate64::FromMillimeters(2'000, 0, 0),
                                                   {0, 0, 1},
                                                   TacticalCoverStance::Crouched},
                                TacticalCoverPoint{TacticalCoverId::Create(2).Value(),
                                                   *protectedAnchor,
                                                   Math::WorldCoordinate64::FromMillimeters(4'000, 0, 0),
                                                   {0, 0, 1},
                                                   TacticalCoverStance::Crouched}};
        TacticalCoverRegistry cover;
        if (cover.Publish(scene, 1, points).HasError())
            return false;
        const auto captured = cover.Capture(scene, points.size());
        if (captured.HasError() || captured.Value().size() != points.size())
            return false;
        const auto route = QueryStageId::Create(1).Value();
        const auto sight = QueryStageId::Create(2).Value();
        const auto contention = QueryStageId::Create(3).Value();
        const std::array observations{Examples::GuardCoverObservation{captured.Value()[0].handle,
                                                                      {.stage = route, .filter = true, .metric = 2.0},
                                                                      {.stage = sight, .filter = true}},
                                      Examples::GuardCoverObservation{captured.Value()[1].handle,
                                                                      {.stage = route, .filter = true, .metric = 4.0},
                                                                      {.stage = sight, .filter = false}}};
        const auto selected = Examples::TrySelectGuardCover(cover, scene, *guard, observations, contention);
        if (selected.HasError() || !selected.Value() || selected.Value()->destination != points[1].position)
            return false;
        const auto position = selected.Value()->destination.Millimeters();
        std::cout << "Reserved cover " << selected.Value()->reservation.cover.id.Value() << " at " << position[0] << ", " << position[1]
                  << ", " << position[2] << " mm\n";
        return cover.Release(selected.Value()->reservation);
    }
}  // namespace

int main() {
    const auto scene = CreateScene();
    if (scene.HasError() || !Run(scene.Value()->View())) {
        std::cerr << "The guard cover fixture could not select and release its protected point.\n";
        return 1;
    }
    return 0;
}
