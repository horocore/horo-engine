#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "support/AllocationProbe.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>
#include <span>
#include <utility>
#include <vector>

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

    /** @brief Metadata-only contiguous canonical fields; these unit layouts make no native reflection claim. */
    MaterialBindingLayout ContiguousLayout(bool columnMajor) {
        auto layout = InstanceLayout(columnMajor);
        layout.parameters[0].byteOffset = 0;
        layout.parameters[0].arrayStride = 64;
        layout.parameters[1].byteOffset = 8192;
        layout.parameters[1].arrayStride = 16;
        return layout;
    }

    /** @brief Creates non-affine nonsymmetric matrices and bit-sensitive finite colors before measured calls. */
    std::vector<VfxPackedDrawInstance> Inputs(std::size_t count) {
        std::vector<VfxPackedDrawInstance> instances(count);
        for (std::size_t instance = 0; instance < count; ++instance) {
            for (std::size_t component = 0; component < 16; ++component)
                instances[instance].clipFromLocal[component] = static_cast<float>(instance * 32 + component + 1) * .125F;
            instances[instance].clipFromLocal[0] = -0.F;
            instances[instance].linearColor = {-0.F, -2.F, std::numeric_limits<float>::max() * .5F,
                                               std::numeric_limits<float>::denorm_min()};
        }
        return instances;
    }

    /** @brief Calls the unchanged generic material writer directly, with its documented logical row-major component input. */
    std::vector<std::byte> GenericOracle(const MaterialBindingLayout &layout, std::span<const VfxPackedDrawInstance> instances) {
        std::array<float, MaximumVfxDrawInstances * 16> logicalMatrices{};
        std::array<float, MaximumVfxDrawInstances * 4> logicalColors{};
        for (std::size_t instance = 0; instance < instances.size(); ++instance) {
            for (std::size_t row = 0; row < 4; ++row)
                for (std::size_t column = 0; column < 4; ++column)
                    logicalMatrices[instance * 16 + row * 4 + column] = instances[instance].clipFromLocal[column * 4 + row];
            std::copy(instances[instance].linearColor.begin(), instances[instance].linearColor.end(), logicalColors.begin() + instance * 4);
        }
        const std::array values{MaterialParameterValue{VfxShaderParameterIds::ClipFromLocal, std::span<const float>{logicalMatrices}},
                                MaterialParameterValue{VfxShaderParameterIds::InstanceColor, std::span<const float>{logicalColors}}};
        auto result = PackMaterialParameterBinding(layout, VfxShaderParameterIds::Instances, values);
        INFO((result.HasError() ? result.ErrorValue().message : "Generic oracle accepted"));
        REQUIRE(result.HasValue());
        return std::move(result.Value().bytes);
    }

    /** @brief Checks exact generic bytes, zero inactive lanes/gaps and untouched suffix without allocation in either eligible path. */
    void CheckOracle(const MaterialBindingLayout &layout, std::span<const VfxPackedDrawInstance> instances) {
        const auto expected = GenericOracle(layout, instances);
        std::vector<std::byte> destination(expected.size() + 31, std::byte{0xA5});
        auto result = Result<std::size_t>::Success(0);
        std::size_t allocations{};
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            result = PackVfxShaderParameters(layout, instances, destination);
            allocations = measurement.Snapshot().requests;
        }
        INFO((result.HasError() ? result.ErrorValue().message : "VFX serializer accepted"));
        REQUIRE(result.HasValue());
        CHECK(result.Value() == expected.size());
        CHECK(allocations == 0);
        CHECK(std::ranges::equal(std::span{destination}.first(expected.size()), expected));
        CHECK(std::ranges::all_of(std::span{destination}.subspan(expected.size()), [](auto byte) {
            return byte == std::byte{0xA5};
        }));
    }

    /** @brief Checks typed rejection and every retained destination byte independently of the optimized serializer. */
    void CheckRejected(const MaterialBindingLayout &layout, std::span<const VfxPackedDrawInstance> instances,
                       std::span<std::byte> destination, const ErrorCodeDescriptor &expected) {
        const std::vector<std::byte> before(destination.begin(), destination.end());
        const auto result = PackVfxShaderParameters(layout, instances, destination);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        CHECK(std::ranges::equal(destination, before));
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

TEST_CASE("VFX regular serialization matches independent generic packing for both matrix orders and full lane ranges",
          "[vfx][draw][reflection][packing]") {
    const bool columnMajor = GENERATE(true, false);
    const auto count = GENERATE(std::size_t{1}, std::size_t{127}, std::size_t{128});
    auto layout = ContiguousLayout(columnMajor);
    SECTION("canonical separate arrays") {}
    SECTION("reversed separate arrays") {
        layout.parameters[0].byteOffset = 2048;
        layout.parameters[1].byteOffset = 0;
    }
    SECTION("gaps and leading padding") {
        layout.parameters[0].byteOffset = 32;
        layout.parameters[1].byteOffset = 8256;
    }
    SECTION("valid interleaved arrays use generic fallback") {
        layout.parameters[0].arrayStride = 80;
        layout.parameters[1].arrayStride = 80;
        layout.parameters[1].byteOffset = 64;
    }
    SECTION("padded physical arrays use generic fallback") {
        layout = InstanceLayout(columnMajor);
    }
    SECTION("padded matrix vectors use generic fallback") {
        layout.parameters[0].matrixStride = 32;
        layout.parameters[0].arrayStride = 128;
        layout.parameters[1].byteOffset = 16384;
    }
    SECTION("unaligned disjoint fields retain generic behavior") {
        ++layout.parameters[0].byteOffset;
        ++layout.parameters[1].byteOffset;
    }
    const auto instances = Inputs(count);
    CheckOracle(layout, instances);
}

TEST_CASE("VFX serializer preserves typed failure ordering and destination bytes", "[vfx][draw][reflection][packing]") {
    auto layout = ContiguousLayout(true);
    auto instances = Inputs(129);
    std::size_t count = 1;
    std::array<std::byte, 10272> destination;
    destination.fill(std::byte{0xA5});
    std::span<std::byte> output{destination};
    const ErrorCodeDescriptor *expected = &RenderGraphExecutionErrors::InvalidGraph;
    SECTION("empty input") {
        count = 0;
    }
    SECTION("excess input") {
        count = 129;
    }
    SECTION("nonfinite final matrix") {
        count = 128;
        instances[127].clipFromLocal[15] = std::numeric_limits<float>::infinity();
    }
    SECTION("nonfinite final color") {
        count = 128;
        instances[127].linearColor[3] = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("nonfinite wins over short output") {
        instances[0].linearColor[0] = -std::numeric_limits<float>::infinity();
        output = output.first(1);
    }
    SECTION("short output") {
        output = output.first(10239);
        expected = &MaterialBindingErrors::InvalidDescriptor;
    }
    SECTION("overlap in inactive physical lanes still rejects") {
        layout.parameters[1].byteOffset = 4096;
        expected = &MaterialBindingErrors::InvalidDescriptor;
    }
    SECTION("overflowed final offset") {
        layout.parameters[0].byteOffset = std::numeric_limits<std::uint32_t>::max();
        expected = &MaterialBindingErrors::InvalidDescriptor;
    }
    SECTION("invalid matrix stride") {
        layout.parameters[0].matrixStride = 8;
        expected = &MaterialBindingErrors::InvalidDescriptor;
    }
    SECTION("duplicate semantic field") {
        layout.parameters.push_back(layout.parameters[0]);
    }
    SECTION("wrong semantic array count") {
        layout.parameters[0].arrayCount = 127;
    }
    SECTION("resource failure wins over nonfinite input") {
        layout.targetBindings.push_back(layout.targetBindings[0]);
        instances[0].linearColor[0] = std::numeric_limits<float>::infinity();
        expected = &MaterialBindingErrors::InvalidDescriptor;
    }
    CheckRejected(layout, std::span{instances}.first(count), output, *expected);
}

TEST_CASE("VFX serializer preserves copied instance and output alias behavior", "[vfx][draw][reflection][packing]") {
    const bool columnMajor = GENERATE(true, false);
    const auto count = GENERATE(std::size_t{1}, std::size_t{127}, std::size_t{128});
    const auto layout = ContiguousLayout(columnMajor);
    auto instances = Inputs(129);  // Real owned storage includes both complete output and retained suffix.
    const auto expected = GenericOracle(layout, std::span{instances}.first(count));
    auto destination = std::as_writable_bytes(std::span{instances});
    const std::vector<std::byte> suffix(destination.begin() + expected.size(), destination.end());
    const auto result = PackVfxShaderParameters(layout, std::span{instances}.first(count), destination);
    REQUIRE(result.HasValue());
    CHECK(result.Value() == expected.size());
    CHECK(std::ranges::equal(destination.first(expected.size()), expected));
    CHECK(std::ranges::equal(destination.subspan(expected.size()), suffix));
}

TEST_CASE("VFX descriptor metadata overlap retains generic rejection without changing metadata", "[vfx][draw][reflection][packing]") {
    auto layout = ContiguousLayout(true);
    const auto extent = VfxShaderParameterBytes(layout);
    REQUIRE(extent.HasValue());
    layout.parameters.resize((extent.Value() + sizeof(ShaderReflectedParameter) - 1) / sizeof(ShaderReflectedParameter));
    for (std::size_t i = 2; i < layout.parameters.size(); ++i)
        layout.parameters[i].active = false;
    const auto instances = Inputs(1);
    auto destination = std::as_writable_bytes(std::span{layout.parameters}).first(extent.Value());
    CheckRejected(layout, instances, destination, MaterialBindingErrors::InvalidDescriptor);
}
