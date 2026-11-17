#pragma once
/** @file CharacterReferenceSceneCorpus.h @brief Versioned asset-light Character scene identities and bounded execution recipes. */
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Horo::Character::Corpus {
    inline constexpr std::uint32_t Version = 1;
    inline constexpr std::size_t MaximumControllers = 8;
    inline constexpr std::size_t MaximumTicks = 4;

    /** @brief Stable recipe discriminator; the shared native runner owns its authored geometry and outcome oracle. */
    enum class ReferenceScene : std::uint8_t {
        Ramp,
        Stair,
        Ledge,
        Corner,
        Seam,
        Ceiling,
        ThinWall,
        Filter,
        HighSpeed,
        Flat,
        PlatformTranslation,
        PlatformRotation,
        Crowd,
        DynamicAwake,
        DynamicSleeping
    };
    /** @brief Explicit semantic support; no recipe promises unimplemented peer avoidance or Character push/reaction. */
    enum class Semantics : std::uint8_t {
        StaticQueries,
        PredictedKinematicSupport,
        IndependentQueryControllers,
        DynamicObstacleOnly
    };

    /** @brief Immutable recipe identity and finite schedule. Dimensions and SI golds live in the shared authored runner. */
    struct Recipe final {
        ReferenceScene scene;
        std::string_view id;
        Semantics semantics;
        std::uint8_t ticks;
        std::uint8_t controllers;
        float positionToleranceMeters{0.005F};
        float replayToleranceMeters{0.00001F};
    };

    inline constexpr std::array
        Recipes{Recipe{ReferenceScene::Ceiling, "ceiling", Semantics::StaticQueries, 1, 1},
                Recipe{ReferenceScene::Corner, "corner", Semantics::StaticQueries, 1, 1},
                Recipe{ReferenceScene::Crowd, "crowd-query-only", Semantics::IndependentQueryControllers, 4, 8},
                Recipe{ReferenceScene::DynamicAwake, "dynamic-awake-obstacle", Semantics::DynamicObstacleOnly, 4, 1},
                Recipe{ReferenceScene::DynamicSleeping, "dynamic-sleeping-obstacle", Semantics::DynamicObstacleOnly, 4, 1},
                Recipe{ReferenceScene::Filter, "filter", Semantics::StaticQueries, 1, 1},
                Recipe{ReferenceScene::Flat, "flat", Semantics::StaticQueries, 4, 1},
                Recipe{ReferenceScene::HighSpeed, "high-speed", Semantics::StaticQueries, 1, 1},
                Recipe{ReferenceScene::Ledge, "ledge", Semantics::StaticQueries, 1, 1},
                Recipe{ReferenceScene::PlatformRotation, "platform-rotation", Semantics::PredictedKinematicSupport, 3, 1},
                Recipe{ReferenceScene::PlatformTranslation, "platform-translation", Semantics::PredictedKinematicSupport, 3, 1},
                Recipe{ReferenceScene::Ramp, "ramp", Semantics::StaticQueries, 1, 1},
                Recipe{ReferenceScene::Seam, "seam", Semantics::StaticQueries, 1, 1},
                Recipe{ReferenceScene::Stair, "stair", Semantics::StaticQueries, 1, 1},
                Recipe{ReferenceScene::ThinWall, "thin-wall", Semantics::StaticQueries, 1, 1}};

    /** @brief Finds a stable scene identity without filesystem or backend discovery. */
    inline const Recipe *Find(const ReferenceScene scene) {
        for (const auto &recipe : Recipes)
            if (recipe.scene == scene)
                return &recipe;
        return nullptr;
    }
}  // namespace Horo::Character::Corpus
