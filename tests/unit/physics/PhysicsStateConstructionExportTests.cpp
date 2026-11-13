#include "AllocationProbe.h"
#include "PhysicsStateConstructionCodec.h"
#include "PhysicsStateConstructionStorage.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Physics::StateDetail {
    namespace {
        constexpr PhysicsStateFpControl Profile{PhysicsStateFpArchitecture::ArmFpcr, 0};
        constexpr PhysicsStateConstructionLimits Limits{8, 8, 4, 8, 65536};

        template <class T> T TakeExport(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().message : "success"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        struct ExportStorage final {
            std::array<PhysicsStateShapeRecipe, 8> nodes;
            std::array<PhysicsStateShapeRecipeEdge, 8> edges;
            std::array<PhysicsStateShapeRecipeRoot, 4> roots;
            std::array<std::uint32_t, 8> map;
            std::array<ConstructionRecipeExportFrame, 8> frames;

            ConstructionRecipeExportStorage View() {
                return {nodes, edges, roots, map, frames};
            }
        };

        ConstructionRecipeLease Parent(ConstructionRecipePool &pool, bool sphereFirst) {
            const auto box = PhysicsShapeDescriptor{PhysicsBoxShape{}};
            const auto sphere = PhysicsShapeDescriptor{PhysicsSphereShape{}};
            auto first = TakeExport(pool.AddAnalytic(sphereFirst ? sphere : box, Profile));
            auto second = TakeExport(pool.AddAnalytic(sphereFirst ? box : sphere, Profile));
            std::array<ConstructionRecipeChild, 2> edges{{{sphereFirst ? second : first, {}, 1}, {sphereFirst ? first : second, {}, 2}}};
            return TakeExport(pool.AddCompound(edges, Profile));
        }
    }  // namespace

    TEST_CASE("Canonical recipe export ignores actual pool allocation order and retains retired children",
              "[unit][physics][state][construction-export]") {
        auto first = TakeExport(ConstructionRecipePool::Prepare(Limits, Profile));
        auto second = TakeExport(ConstructionRecipePool::Prepare(Limits, Profile));
        auto churn = TakeExport(second->AddAnalytic(PhysicsCapsuleShape{}, Profile));
        auto a = Parent(*first, false);
        auto b = Parent(*second, true);
        churn.Reset();
        TakeExport(second->Collect(8));
        ExportStorage x, y;
        std::array<ConstructionRecipeRootBinding, 1> rootsX{{{7, a}}}, rootsY{{{7, b}}};
        const auto before = Tests::AllocationProbe::Count();
        auto exportedX = first->Export(rootsX, x.View());
        auto exportedY = second->Export(rootsY, y.View());
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(exportedX.HasValue());
        REQUIRE(exportedY.HasValue());
        CHECK(before == after);
        CHECK(exportedX.Value().nodes.size() == 3);
        CHECK(exportedX.Value().edges.size() == 2);
        CHECK(exportedX.Value().roots[0].recipe == 2);
        std::array<std::byte, 1024> wireX{}, wireY{};
        std::array<std::uint32_t, 8> depthsX{}, depthsY{};
        const auto bytesX = TakeExport(EncodeConstructionShapes(exportedX.Value(), Limits, Profile, depthsX, wireX));
        const auto bytesY = TakeExport(EncodeConstructionShapes(exportedY.Value(), Limits, Profile, depthsY, wireY));
        REQUIRE(bytesX == bytesY);
        CHECK(std::equal(wireX.begin(), wireX.begin() + bytesX, wireY.begin()));
    }

    TEST_CASE("Recipe export enforces exact owner roots capacities and scratch disjointness",
              "[unit][physics][state][construction-export]") {
        auto pool = TakeExport(ConstructionRecipePool::Prepare(Limits, Profile));
        auto foreign = TakeExport(ConstructionRecipePool::Prepare(Limits, Profile));
        auto local = TakeExport(pool->AddAnalytic(PhysicsBoxShape{}, Profile));
        auto other = TakeExport(foreign->AddAnalytic(PhysicsBoxShape{}, Profile));
        ExportStorage storage;
        std::array<ConstructionRecipeRootBinding, 2> roots{{{2, local}, {1, local}}};
        CHECK(pool->Export(roots, storage.View()).HasError());
        roots[1].id = 2;
        CHECK(pool->Export(roots, storage.View()).HasError());
        roots[1].id = 3;
        roots[1].recipe = other;
        CHECK(pool->Export(roots, storage.View()).HasError());
        roots[1].recipe = local;
        auto shortStorage = storage.View();
        shortStorage.nodeMap = shortStorage.nodeMap.first(7);
        CHECK(pool->Export(roots, shortStorage).HasError());
        auto aliased = storage.View();
        aliased.nodeMap = {reinterpret_cast<std::uint32_t *>(storage.frames.data()), 8};
        CHECK(pool->Export(roots, aliased).HasError());
        auto valid = TakeExport(pool->Export(roots, storage.View()));
        CHECK(valid.nodes.size() == 1);
        CHECK(valid.roots.size() == 2);
        CHECK(valid.roots[0].recipe == valid.roots[1].recipe);
    }
}  // namespace Horo::Physics::StateDetail
