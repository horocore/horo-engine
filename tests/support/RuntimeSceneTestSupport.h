#pragma once

#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace RuntimeSceneTestSupport {
    using namespace Horo;
    using namespace Horo::Runtime;

    inline void Check(bool condition) {
        REQUIRE((condition));
    }

    inline RuntimeEntityDefinition Entity(std::uint64_t id, std::optional<std::uint64_t> parent = std::nullopt) {
        RuntimeEntityDefinition entity;
        entity.object = SceneObjectId{id};
        if (parent)
            entity.parent = SceneObjectId{*parent};
        return entity;
    }

    inline RuntimeSceneDefinition Definition(std::uint64_t revision = 1) {
        SceneDefinitionBuilder builder{SceneDefinitionId{7}, SceneDefinitionRevision{revision}};
        builder.Add(Entity(1));
        builder.Add(Entity(2, 1));
        auto built = std::move(builder).Build();
        Check(built.HasValue());
        return std::move(built).Value();
    }

    inline FrameContext Context(const CancellationToken &token) {
        return FrameContext{1, {}, 0.0, 0, {}, false, token};
    }

}  // namespace RuntimeSceneTestSupport
