#include "support/AllocationProbe.h"
#include "support/renderer/MultiviewTestSupport.h"

#include <algorithm>
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
        std::array<std::byte, 256> slot;
        slot.fill(std::byte{0xA5});
        const auto before = slot;
        const auto allocations = Horo::Tests::AllocationProbe::Count();
        const auto written = PackMultiviewShaderParameters(layout, views, model, layers, slot);
        const auto after = Horo::Tests::AllocationProbe::Count();
        REQUIRE(written.HasValue());
        REQUIRE(after == allocations);
        REQUIRE(written.Value() == packed.Value().bytes.size());
        REQUIRE(std::ranges::equal(std::span{slot}.first(written.Value()), packed.Value().bytes));
        REQUIRE(std::ranges::equal(std::span{slot}.subspan(written.Value()), std::span{before}.subspan(written.Value())));
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
        std::array<std::byte, 256> slot;
        slot.fill(std::byte{0xA5});
        const auto original = slot;
        REQUIRE(PackMultiviewShaderParameters(layout, views, Math::Mat4::Identity(), layers, slot).HasError());
        REQUIRE(slot == original);
    }

    TEST_CASE("Multiview environment parameters explicitly clear lighting and preserve the projection ABI",
              "[renderer][multiview][environment][shader]") {
        using namespace MultiviewShaderParameterIds;
        auto layout = MultiviewTest::Layout();
        const std::array environmentFields{ShaderReflectedParameter{NormalToWorld, Buffer, ShaderValueType::Float32, 4, 4, 1, 224, 0, 16,
                                                                    true, true},
                                           ShaderReflectedParameter{EnvironmentAmbient, Buffer, ShaderValueType::Float32, 1, 4, 1, 288, 0,
                                                                    0, false, true},
                                           ShaderReflectedParameter{EnvironmentDirectional, Buffer, ShaderValueType::Float32, 1, 4, 1, 304,
                                                                    0, 0, false, true},
                                           ShaderReflectedParameter{EnvironmentDirection, Buffer, ShaderValueType::Float32, 1, 4, 1, 320, 0,
                                                                    0, false, true}};
        layout.parameters.insert(layout.parameters.end(), environmentFields.begin(), environmentFields.end());
        const std::array views{MultiviewSceneView{0, Math::Mat4::Identity()}, MultiviewSceneView{1, Math::Mat4::Identity()}};
        const std::array layers{0U, 0U};
        auto model = Math::Mat4::Identity();
        model.values[0] = 2.0F;
        model.values[4] = 1.0F;
        const auto baseline = PackMultiviewShaderParameters(MultiviewTest::Layout(), views, model, layers);
        REQUIRE(baseline.HasValue());
        REQUIRE(PackMultiviewShaderParameters(layout, views, model, layers).HasError());
        const auto size = MultiviewEnvironmentParameterBytes(layout, views.size());
        REQUIRE(size.HasValue());
        MaterialParameterBinding buffer{std::vector<std::byte>(size.Value(), std::byte{0xA5})};
        MultiviewEnvironmentLighting light{.ambient = Math::Vec3{0.25F, 0.5F, -0.125F},
                                           .directional = RenderLight{.color = {0.5F, 1.0F, 2.0F}, .intensity = 2.0F, .stableIdentity = 1}};
        const auto allocations = Horo::Tests::AllocationProbe::Count();
        const auto packed = PackMultiviewEnvironmentParameters(layout, views, model, layers, light, buffer.bytes);
        const auto after = Horo::Tests::AllocationProbe::Count();
        REQUIRE(packed.HasValue());
        CHECK(after == allocations);
        CHECK(std::ranges::equal(baseline.Value().bytes, std::span{buffer.bytes}.first(baseline.Value().bytes.size())));
        CHECK(Read<float>(buffer, 224) == 0.5F);
        CHECK(Read<float>(buffer, 228) == -0.5F);
        CHECK(Read<float>(buffer, 296) == -0.125F);
        CHECK(Read<float>(buffer, 300) == 1.0F);
        CHECK(Read<float>(buffer, 312) == 4.0F);
        CHECK(Read<float>(buffer, 328) == -1.0F);

        const auto prior = buffer.bytes;
        light.directional->kind = RenderLightKind::Point;
        REQUIRE(PackMultiviewEnvironmentParameters(layout, views, model, layers, light, buffer.bytes).HasError());
        CHECK(buffer.bytes == prior);
        REQUIRE(PackMultiviewEnvironmentParameters(layout, views, model, layers, {}, buffer.bytes).HasValue());
        CHECK(std::ranges::all_of(std::span{buffer.bytes}.subspan(288), [](const std::byte channel) {
            return channel == std::byte{};
        }));
    }
}  // namespace Horo::Render
