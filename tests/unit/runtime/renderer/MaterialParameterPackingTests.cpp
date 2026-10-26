#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "MaterialBindingTestFixture.h"
#include "support/AllocationProbe.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace Horo;
using namespace Horo::Render;
using namespace Horo::Tests::MaterialBindingFixture;

TEST_CASE("Caller-owned canonical packing is byte-equivalent and preserves all bytes on rejection", "[renderer][material][packing]") {
    auto layout = Descriptor().layout;
    layout.parameters = {{{1}, {1}, ShaderValueType::Float32, 2, 3, 2, 16, 64, 16, true, true}};
    const std::array<float, 12> components{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    std::vector values{MaterialParameterValue{{1}, std::span<const float>{components}}};
    std::array<std::byte, 128> destination;
    destination.fill(std::byte{0xA5});
    const auto before = destination;
    SECTION("valid exact matrix-array ABI and retained suffix") {
        const auto owned = PackMaterialParameterBinding(layout, {1}, values);
        REQUIRE(owned.HasValue());
        bool succeeded = true;
        const auto allocations = Horo::Tests::AllocationProbe::Count();
        for (unsigned frame = 0; frame < 64; ++frame) {
            const auto written = PackMaterialParameterBinding(layout, {1}, values, destination);
            succeeded &= written.HasValue() && written.Value() == owned.Value().bytes.size();
        }
        const auto after = Horo::Tests::AllocationProbe::Count();
        REQUIRE(succeeded);
        REQUIRE(after == allocations);
        REQUIRE(std::ranges::equal(std::span{destination}.first(120), owned.Value().bytes));
        REQUIRE(std::ranges::equal(std::span{destination}.subspan(120), std::span{before}.subspan(120)));
        return;
    }
    SECTION("undersized destination") {
        ErrorIs(PackMaterialParameterBinding(layout, {1}, values, std::span{destination}.first(119)),
                MaterialBindingErrors::InvalidDescriptor);
        REQUIRE(destination == before);
        return;
    }
    SECTION("duplicate final value") {
        values.push_back(values.front());
    }
    SECTION("missing final component") {
        values[0].components = std::span<const float>{components}.first(11);
    }
    SECTION("overflowed reflected tail") {
        layout.parameters[0].byteOffset = std::numeric_limits<std::uint32_t>::max();
    }
    SECTION("occupied-byte overlap") {
        auto second = layout.parameters.front();
        second.id = {2};
        layout.parameters.push_back(second);
        values.push_back({{2}, std::span<const float>{components}});
    }
    ErrorIs(PackMaterialParameterBinding(layout, {1}, values, destination), MaterialBindingErrors::InvalidDescriptor);
    REQUIRE(destination == before);
}

TEST_CASE("Caller-owned packing rejects source aliasing before zeroing its input", "[renderer][material][packing]") {
    auto layout = Descriptor().layout;
    std::array<float, 4> storage{1, 2, 3, 4};
    const auto original = storage;
    const std::array values{MaterialParameterValue{{1}, std::span<const float>{storage}.first(3)}};
    ErrorIs(PackMaterialParameterBinding(layout, {1}, values, std::as_writable_bytes(std::span{storage})),
            MaterialBindingErrors::InvalidDescriptor);
    REQUIRE(storage == original);
}
