#include "support/renderer/MultiviewTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace Horo::Render {
    namespace {
        template <typename Scalar> Scalar Read(const MaterialParameterBinding &binding, const std::size_t offset) {
            Scalar value{};
            std::memcpy(&value, binding.bytes.data() + offset, sizeof(value));
            return value;
        }
    }  // namespace

    TEST_CASE("Multiview uses canonical reflected matrix and routing packing", "[renderer][multiview][shader]") {
        bool columnMajor = true;
        SECTION("column major") {}
        SECTION("row major") {
            columnMajor = false;
        }
        auto layout = MultiviewTest::Layout(columnMajor);
        auto first = Math::Mat4::Identity();
        first.values[12] = 0.25F;
        auto second = Math::Mat4::Identity();
        second.values[13] = -0.5F;
        auto model = Math::Mat4::Identity();
        model.values[14] = -3.0F;
        const std::array views{MultiviewSceneView{0, first}, MultiviewSceneView{1, second}};
        const std::array<std::uint32_t, 2> layers{1, 0};
        const auto packed = PackMultiviewShaderParameters(layout, views, model, layers);
        REQUIRE(packed.HasValue());
        CHECK(packed.Value().bytes.size() == ComputeMaterialParameterBufferByteSize(layout, {1}).Value());
        const auto translation = [columnMajor](const std::size_t row) {
            return columnMajor ? 48 + row * 4 : row * 16 + 12;
        };
        CHECK(Read<float>(packed.Value(), translation(0)) == 0.25F);
        CHECK(Read<float>(packed.Value(), 64 + translation(1)) == -0.5F);
        CHECK(Read<float>(packed.Value(), 128 + translation(2)) == -3.0F);
        CHECK(Read<std::uint32_t>(packed.Value(), 192) == 1);
        CHECK(Read<std::uint32_t>(packed.Value(), 208) == 0);
        CHECK(Read<std::uint32_t>(packed.Value(), 196) == 0);
    }

    TEST_CASE("Multiview parameter adapter rejects incomplete views and malformed final layouts", "[renderer][multiview][shader]") {
        auto layout = MultiviewTest::Layout();
        std::array views{MultiviewSceneView{0, Math::Mat4::Identity()}, MultiviewSceneView{1, Math::Mat4::Identity()}};
        std::array<std::uint32_t, 2> layers{};
        SECTION("wrong dense index") {
            views[1].index = 0;
        }
        SECTION("missing matrix capacity") {
            layout.parameters[0].arrayCount = 1;
        }
        SECTION("missing layer field") {
            layout.parameters.pop_back();
        }
        SECTION("overlapping fields") {
            layout.parameters[2].byteOffset = 0;
        }
        SECTION("invalid matrix stride") {
            layout.parameters[0].matrixStride = 4;
        }
        REQUIRE(PackMultiviewShaderParameters(layout, views, Math::Mat4::Identity(), layers).HasError());
    }
}  // namespace Horo::Render
