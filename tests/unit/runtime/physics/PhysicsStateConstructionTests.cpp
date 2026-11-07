#include "AllocationProbe.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsStateConstruction.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Physics {
    namespace {
        // Pure model controls; this fixture grants no native/image/profile readiness.
        constexpr PhysicsStateFpControl ModelFp{PhysicsStateFpArchitecture::ArmFpcr, 0};

        struct RecipeFixture final {
            std::array<PhysicsStateShapeRecipe, 3> nodes{{{ModelFp, PhysicsShapeDescriptor{PhysicsBoxShape{}}},
                                                          {ModelFp, PhysicsStateCompoundRecipe{0, 1}},
                                                          {ModelFp, PhysicsStateCompoundRecipe{1, 1}}}};
            std::array<PhysicsStateShapeRecipeEdge, 2> edges{{{0, {}, 1}, {1, {}, 1}}};
            std::array<PhysicsStateShapeRecipeRoot, 1> roots{{{900, 2}}};
            std::array<std::uint32_t, 3> scratch{};
            PhysicsStateConstructionLimits limits{3, 2, 1, 3, 8192};

            PhysicsStateShapeRecipes View() const noexcept {
                return {nodes, edges, roots};
            }

            Result<void> Validate() {
                return ValidatePhysicsStateShapeRecipes(View(), limits, ModelFp, scratch);
            }
        };

        void Invalid(const Result<void> &result) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());
        }
    }  // namespace

    TEST_CASE("Canonical construction graph admits nested original edges without allocation", "[unit][physics][state][construction]") {
        RecipeFixture f;
        const auto before = Tests::AllocationProbe::Count();
        auto result = f.Validate();
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(result.HasValue());
        CHECK(after == before);
        CHECK(f.edges[0].userData == 1);
        CHECK(f.edges[1].userData == 1);
    }

    TEST_CASE("Construction graph rejects forward cyclic and out-of-range child references", "[unit][physics][state][construction]") {
        RecipeFixture f;
        for (auto bad : {1U, 2U, 0xFFFFFFFFU}) {
            f.edges[0].child = bad;
            Invalid(f.Validate());
        }
    }

    TEST_CASE("Construction graph requires dense owned edges and reachable nodes", "[unit][physics][state][construction]") {
        RecipeFixture f;
        SECTION("overlapping edge slices") {
            std::get<PhysicsStateCompoundRecipe>(f.nodes[2].geometry).begin = 0;
            Invalid(f.Validate());
        }
        SECTION("orphan recipe") {
            f.roots[0].recipe = 1;
            Invalid(f.Validate());
        }
        SECTION("unowned edge") {
            f.nodes[2].geometry = PhysicsShapeDescriptor{PhysicsSphereShape{}};
            Invalid(f.Validate());
        }
    }

    TEST_CASE("Construction graph validates FP provenance rather than normalizing it", "[unit][physics][state][construction]") {
        RecipeFixture f;
        f.nodes[1].floatingPoint.value = 1;
        auto result = f.Validate();
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == PhysicsErrors::OperationUnsupported.code.Value());
        CHECK(f.nodes[1].floatingPoint.value == 1);
    }

    TEST_CASE("Construction graph bounded depth byte and scratch admission precedes writing", "[unit][physics][state][construction]") {
        RecipeFixture f;
        SECTION("depth") {
            f.limits.maximumDepth = 2;
            CHECK(f.Validate().HasError());
        }
        SECTION("byte budget") {
            f.limits.maximumBytes = 1;
            CHECK(f.Validate().HasError());
        }
        SECTION("node limit") {
            f.limits.maximumNodes = 2;
            CHECK(f.Validate().HasError());
        }
        SECTION("short scratch") {
            CHECK(ValidatePhysicsStateShapeRecipes(f.View(), f.limits, ModelFp, std::span{f.scratch}.first(2)).HasError());
        }
    }

    TEST_CASE("Construction graph rejects malformed primitive and root identity", "[unit][physics][state][construction]") {
        RecipeFixture f;
        SECTION("invalid radius") {
            f.nodes[0].geometry = PhysicsShapeDescriptor{PhysicsSphereShape{-1}};
            Invalid(f.Validate());
        }
        SECTION("zero root identity") {
            f.roots[0].shape = 0;
            Invalid(f.Validate());
        }
        SECTION("root out of bounds") {
            f.roots[0].recipe = 3;
            Invalid(f.Validate());
        }
    }
}  // namespace Horo::Physics
