#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "support/AllocationProbe.h"

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

using namespace Horo;
using namespace Horo::Render;

namespace {
    /** @brief Deliberately padded final-target ABI with separate matrix and color arrays. */
    MaterialBindingLayout InstanceLayout(bool columnMajor) {
        MaterialBindingLayout layout;
        layout.resources = {
            {{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex, true}};
        layout.targetBindings = {{{1}, 0, 0, 0, "", {}, true}};
        layout.parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 128, 16, 80, 16, columnMajor, true},
                             {{2}, {1}, ShaderValueType::Float32, 1, 4, 128, 10'256, 32, 0, false, true}};
        return layout;
    }

    float FloatAt(std::span<const std::byte> bytes, std::size_t offset) {
        std::array<std::byte, 4> value{};
        std::copy_n(bytes.begin() + offset, 4, value.begin());
        return std::bit_cast<float>(value);
    }
}  // namespace

TEST_CASE("VFX material adapter honors reflected matrix order and inactive lane padding", "[vfx][draw][reflection]") {
    const bool columnMajor = GENERATE(true, false);
    const auto layout = InstanceLayout(columnMajor);
    VfxPackedDrawInstance instance;
    instance.clipFromLocal = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    instance.linearColor = {0.25F, 0.5F, 0.75F, 1};
    std::array<std::byte, 14'400> storage;
    storage.fill(std::byte{0xA5});
    std::size_t allocations{};
    auto result = Result<std::size_t>::Success(0);
    {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        result = PackVfxShaderParameters(layout, std::span{&instance, 1}, storage);
        allocations = measurement.Snapshot().requests;
    }
    REQUIRE(result.HasValue());
    CHECK(allocations == 0);
    CHECK(FloatAt(storage, 16) == 1);
    CHECK(FloatAt(storage, 20) == (columnMajor ? 2 : 5));
    CHECK(FloatAt(storage, 32) == (columnMajor ? 5 : 2));
    CHECK(FloatAt(storage, 76) == 16);
    CHECK(FloatAt(storage, 10'256) == 0.25F);
    CHECK(FloatAt(storage, 10'268) == 1);
    CHECK(FloatAt(storage, 96) == 0);
    CHECK(FloatAt(storage, 10'288) == 0);
    CHECK(storage[0] == std::byte{0});
    CHECK(storage[result.Value()] == std::byte{0xA5});
}

TEST_CASE("VFX material adapter leaves retained slot bytes unchanged on rejected preparation", "[vfx][draw][reflection]") {
    auto layout = InstanceLayout(true);
    VfxPackedDrawInstance instance;
    std::array<std::byte, 14'400> storage;
    storage.fill(std::byte{0xA5});
    const auto before = storage;
    SECTION("nonfinite payload") {
        instance.clipFromLocal[7] = std::numeric_limits<float>::infinity();
    }
    SECTION("duplicate schema field") {
        layout.parameters[1] = layout.parameters[0];
    }
    SECTION("wrong array extent") {
        layout.parameters[0].arrayCount = 127;
    }
    SECTION("overlapping final target ranges") {
        layout.parameters[1].byteOffset = 16;
    }
    CHECK(PackVfxShaderParameters(layout, std::span{&instance, 1}, storage).HasError());
    CHECK(storage == before);
}
