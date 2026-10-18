#include "RuntimeSceneTestSupport.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace RuntimeSceneTestSupport;

    TEST_CASE("Bulk Scene construction preserves sparse identities components and forward parents", "[unit][runtime][scene]") {
        constexpr std::size_t count = 8192;
        constexpr std::uint64_t base = std::uint64_t{1} << 40U;
        bool parented = false;
        SECTION("flat scene") {}
        SECTION("parent declared after every child") {
            parented = true;
        }
        SceneDefinitionBuilder builder{SceneDefinitionId{7}, SceneDefinitionRevision{1}};
        for (std::size_t index = 0; index < count; ++index) {
            auto entity = Entity(base + (count - index) * 3);
            entity.localTransform.translation.x = static_cast<float>(index);
            if (parented && index + 1 < count)
                entity.parent = SceneObjectId{base + 3};
            if (index == count / 2)
                entity.components.camera = CameraComponent{};
            builder.Add(std::move(entity));
        }
        const auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        const auto created = RuntimeScene::Create(definition.Value(), SceneRuntimeId{11});
        REQUIRE(created.HasValue());
        const auto view = created.Value()->View();
        REQUIRE(view.SlotCount() == count);
        const auto root = view.Find(SceneObjectId{base + 3});
        REQUIRE(root.has_value());
        for (std::size_t index = 0; index < count; ++index) {
            const auto entity = view.EntityAt(index);
            REQUIRE(entity.has_value());
            CHECK(entity->authoredObject == SceneObjectId{base + (count - index) * 3});
            CHECK(entity->localTransform->translation.x == static_cast<float>(index));
            CHECK(entity->components->camera.has_value() == (index == count / 2));
            CHECK(entity->parent == (parented && index + 1 < count ? root : std::nullopt));
        }
        for (const auto index : {std::size_t{0}, count / 2, count - 1})
            CHECK(view.Find(SceneObjectId{base + (count - index) * 3}) == view.EntityAt(index)->entity);
    }

    TEST_CASE("Dynamic create retains duplicate and schema admission after bulk construction", "[unit][runtime][scene]") {
        const auto created = RuntimeScene::Create(Definition(), SceneRuntimeId{11});
        REQUIRE(created.HasValue());
        const auto before = created.Value()->View();
        SceneCommandBuffer commands;
        RuntimeEntityCreateInfo info;
        bool duplicate = true;
        SECTION("existing authored identity") {
            info.authoredObject = SceneObjectId{1};
        }
        SECTION("duplicate within one private candidate") {
            info.authoredObject = SceneObjectId{900};
            static_cast<void>(commands.Create(info));
        }
        SECTION("invalid initial transform") {
            duplicate = false;
            info.localTransform.translation.x = std::numeric_limits<float>::infinity();
        }
        static_cast<void>(commands.Create(info));
        const auto rejected = created.Value()->Commit(commands);
        REQUIRE(rejected.HasError());
        if (duplicate)
            CHECK(rejected.ErrorValue().code.Value() == "scene.object.duplicate");
        CHECK(before.IsCurrent());
        CHECK(before.SlotCount() == 2);
        CHECK_FALSE(before.Find(SceneObjectId{900}).has_value());
    }

}  // namespace
