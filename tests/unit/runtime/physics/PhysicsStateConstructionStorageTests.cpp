#include "AllocationProbe.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateConstructionStorage.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::Physics::StateDetail {
    namespace {
        constexpr PhysicsStateFpControl ModelFp{PhysicsStateFpArchitecture::ArmFpcr, 0};

        std::shared_ptr<ConstructionRecipePool> Pool(std::uint32_t nodes = 2, std::uint32_t edges = 1) {
            auto prepared = ConstructionRecipePool::Prepare({nodes, edges, nodes, nodes, 16384}, ModelFp);
            REQUIRE(prepared.HasValue());
            return std::move(prepared).Value();
        }

        template <class T> T Take(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().message : "success"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    }  // namespace

    TEST_CASE("Actual recipe storage retains a child after its logical owner releases", "[unit][physics][state][recipe-lifetime]") {
        auto pool = Pool();
        auto child = Take(pool->AddAnalytic(PhysicsBoxShape{}, ModelFp));
        std::array<ConstructionRecipeChild, 1> input{{{child, {}, 1}}};
        auto parent = Take(pool->AddCompound(input, ModelFp));
        child.Reset();
        input[0].recipe.Reset();
        CHECK(Take(pool->Collect(2)) == 0);
        CHECK(pool->AddAnalytic(PhysicsSphereShape{}, ModelFp).HasError());
        std::array<ConstructionRecipeChild, 1> output;
        CHECK(Take(pool->CopyChildren(parent, output)) == 1);
        CHECK(std::holds_alternative<PhysicsShapeDescriptor>(Take(pool->Read(output[0].recipe)).geometry));
        CHECK(output[0].userData == 1);
        output[0].recipe.Reset();
        parent.Reset();
        const auto first = Take(pool->Collect(2));
        const auto second = Take(pool->Collect(2));
        CHECK(first + second == 2);
        CHECK(pool->AddAnalytic(PhysicsSphereShape{}, ModelFp).HasValue());
    }

    TEST_CASE("Recipe storage successful construction copies and reads use cold capacity only", "[unit][physics][state][recipe-lifetime]") {
        auto pool = Pool();
        auto child = Take(pool->AddAnalytic(PhysicsBoxShape{}, ModelFp));
        std::array<ConstructionRecipeChild, 1> input{{{child, {}, 1}}}, output;
        const auto before = Tests::AllocationProbe::Count();
        auto parent = pool->AddCompound(input, ModelFp);
        auto copied = pool->CopyChildren(parent.Value(), output);
        auto value = pool->Read(output[0].recipe);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(parent.HasValue());
        REQUIRE(copied.HasValue());
        REQUIRE(value.HasValue());
        CHECK(before == after);
    }

    TEST_CASE("Recipe storage foreign last reader only drops atomic ownership", "[unit][physics][state][recipe-lifetime]") {
        auto pool = Pool(1, 0);
        auto child = Take(pool->AddAnalytic(PhysicsBoxShape{}, ModelFp));
        std::jthread worker([lease = std::move(child)]() mutable {
            lease.Reset();
        });
        worker.join();
        CHECK(pool->AddAnalytic(PhysicsSphereShape{}, ModelFp).HasError());
        CHECK(Take(pool->Collect(1)) == 1);
        CHECK(pool->AddAnalytic(PhysicsSphereShape{}, ModelFp).HasValue());
    }

    TEST_CASE("Recipe storage bounded round robin reclaims beyond retained prefix", "[unit][physics][state][recipe-lifetime]") {
        auto pool = Pool(3, 0);
        auto first = Take(pool->AddAnalytic(PhysicsBoxShape{}, ModelFp));
        auto second = Take(pool->AddAnalytic(PhysicsSphereShape{}, ModelFp));
        auto third = Take(pool->AddAnalytic(PhysicsCapsuleShape{}, ModelFp));
        third.Reset();
        CHECK(Take(pool->Collect(1)) == 0);
        CHECK(Take(pool->Collect(1)) == 0);
        CHECK(Take(pool->Collect(1)) == 1);
        CHECK(pool->Read(first).HasValue());
        CHECK(pool->Read(second).HasValue());
    }

    TEST_CASE("Recipe storage rejects foreign pool FP overflow and bad pose before consumption",
              "[unit][physics][state][recipe-lifetime]") {
        auto pool = Pool();
        auto foreign = Pool();
        auto child = Take(foreign->AddAnalytic(PhysicsBoxShape{}, ModelFp));
        std::array<ConstructionRecipeChild, 1> edges{{{child, {}, 1}}};
        CHECK(pool->AddCompound(edges, ModelFp).HasError());
        CHECK(pool->AddAnalytic(PhysicsBoxShape{}, {PhysicsStateFpArchitecture::ArmFpcr, 1}).HasError());
        auto local = Take(pool->AddAnalytic(PhysicsBoxShape{}, ModelFp));
        edges[0].recipe = local;
        edges[0].localPose.rotation = {0, 0, 0, 0};
        CHECK(pool->AddCompound(edges, ModelFp).HasError());
        CHECK(pool->AddAnalytic(PhysicsSphereShape{}, ModelFp).HasValue());
    }
}  // namespace Horo::Physics::StateDetail
