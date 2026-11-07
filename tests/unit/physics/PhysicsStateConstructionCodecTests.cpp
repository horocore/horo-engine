#include "AllocationProbe.h"
#include "PhysicsStateConstructionCodec.h"

#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Physics::StateDetail {
    namespace {
        constexpr PhysicsStateFpControl ModelFp{PhysicsStateFpArchitecture::ArmFpcr, 0};

        struct CodecFixture final {
            std::array<PhysicsStateShapeRecipe, 2> nodes{{{ModelFp, PhysicsShapeDescriptor{PhysicsStaticPlaneShape{{0, 1, 0}, -0.0F}}},
                                                          {ModelFp, PhysicsStateCompoundRecipe{0, 1}}}};
            std::array<PhysicsStateShapeRecipeEdge, 1> edges{{{0, {}, 1ULL}}};
            std::array<PhysicsStateShapeRecipeRoot, 1> roots{{{700, 1}}};
            std::array<PhysicsStateShapeRecipe, 2> decodedNodes;
            std::array<PhysicsStateShapeRecipeEdge, 1> decodedEdges;
            std::array<PhysicsStateShapeRecipeRoot, 1> decodedRoots;
            std::array<std::uint32_t, 2> depths;
            std::array<std::byte, 512> bytes;
            PhysicsStateConstructionLimits limits{2, 1, 1, 2, 8192};

            Result<std::size_t> Encode() {
                return EncodeConstructionShapes({nodes, edges, roots}, limits, ModelFp, depths, bytes);
            }

            Result<PhysicsStateShapeRecipes> Decode(std::span<const std::byte> input) {
                return DecodeConstructionShapes(input, limits, ModelFp, depths, {decodedNodes, decodedEdges, decodedRoots});
            }
        };
    }  // namespace

    TEST_CASE("Canonical shape subsection preserves exact scalar bits without hot allocation",
              "[unit][physics][state][construction-codec]") {
        CodecFixture f;
        const auto before = Tests::AllocationProbe::Count();
        auto written = f.Encode();
        const auto afterWrite = Tests::AllocationProbe::Count();
        REQUIRE(written.HasValue());
        const auto beforeRead = Tests::AllocationProbe::Count();
        auto decoded = f.Decode(std::span{f.bytes}.first(written.Value()));
        const auto afterRead = Tests::AllocationProbe::Count();
        REQUIRE(decoded.HasValue());
        CHECK(before == afterWrite);
        CHECK(beforeRead == afterRead);
        const auto &plane = std::get<PhysicsStaticPlaneShape>(std::get<PhysicsShapeDescriptor>(decoded.Value().nodes[0].geometry));
        CHECK(std::bit_cast<std::uint32_t>(plane.signedDistanceMeters) == 0x80000000U);
        CHECK(decoded.Value().edges[0].userData == 1ULL);
        CHECK(decoded.Value().roots[0].shape == 700);
    }

    TEST_CASE("Every truncated canonical construction subsection fails without issuing a graph",
              "[unit][physics][state][construction-codec]") {
        CodecFixture f;
        auto written = f.Encode();
        REQUIRE(written.HasValue());
        for (std::size_t prefix = 0; prefix < written.Value(); ++prefix)
            CHECK(f.Decode(std::span{f.bytes}.first(prefix)).HasError());
    }

    TEST_CASE("Construction subsection rejects unknown tags kinds and trailing bytes", "[unit][physics][state][construction-codec]") {
        CodecFixture f;
        auto written = f.Encode();
        REQUIRE(written.HasValue());
        SECTION("tag") {
            f.bytes[0] = std::byte{0};
        }
        SECTION("analytic kind") {
            f.bytes[26] = std::byte{4};
        }
        SECTION("trailing required data") {
            CHECK(f.Decode(std::span{f.bytes}.first(written.Value() + 1)).HasError());
            return;
        }
        CHECK(f.Decode(std::span{f.bytes}.first(written.Value())).HasError());
    }

    TEST_CASE("Construction subsection does not normalize incompatible creation FP", "[unit][physics][state][construction-codec]") {
        CodecFixture f;
        auto written = f.Encode();
        REQUIRE(written.HasValue());
        f.bytes[17] = std::byte{1};
        CHECK(f.Decode(std::span{f.bytes}.first(written.Value())).HasError());
        CHECK(f.bytes[17] == std::byte{1});
    }
}  // namespace Horo::Physics::StateDetail
