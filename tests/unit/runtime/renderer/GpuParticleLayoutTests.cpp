#include "Horo/Runtime/Render/GpuParticleLayout.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    constexpr GpuParticleLayoutLimits Limits{256U * 1024U * 1024U, 256U * 1024U * 1024U};

    GpuParticleLayout Layout(const std::uint32_t capacity, const std::uint32_t custom = 0, const GpuParticleLayoutLimits &limits = Limits) {
        const auto result = BuildGpuParticleLayout(capacity, custom, limits);
        INFO((result.HasError() ? result.ErrorValue().message : "Layout admitted."));
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().IsValid());
        return result.Value();
    }

    void RequireForceFailure(const std::span<const Vfx::CpuParticleForceModule> forces, const std::size_t available,
                             const ErrorCodeDescriptor &error) {
        std::array<std::uint32_t, Vfx::ParticleSimulationDataLimits::ForceModules * GpuParticleForceWords::Count> words;
        words.fill(0xA5A5A5A5U);
        const auto before = words;
        const auto result = PackGpuParticleForces(forces, std::span{words}.first(available));
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == error.code.Value());
        CHECK(words == before);
    }
}  // namespace

TEST_CASE("GPU particle SoA uses explicit aligned word streams", "[unit][renderer][vfx][gpu-layout]") {
    const auto one = Layout(1);
    CHECK(one.streamStrideWords == 4);
    CHECK(one.streamCount == 18);
    CHECK(one.wordCount == 72);
    CHECK(one.byteCount == 288);
    const auto padded = Layout(5, 2);
    CHECK(padded.streamStrideWords == 8);
    CHECK(padded.streamCount == 20);
    CHECK(padded.wordCount == 160);
    CHECK(padded.byteCount == 640);
    for (std::uint32_t stream = 0; stream < padded.streamCount; ++stream) {
        std::uint32_t first = 99, last = 99;
        REQUIRE(TryGpuParticleWordOffset(padded, stream, 0, first));
        REQUIRE(TryGpuParticleWordOffset(padded, stream, 4, last));
        CHECK(first == stream * 8);
        CHECK(first % 4 == 0);
        CHECK(last == first + 4);
        CHECK(last < padded.wordCount);
    }
}

TEST_CASE("GPU particle streams keep canonical values and identities separate", "[unit][renderer][vfx][gpu-layout]") {
    CHECK(static_cast<std::uint32_t>(GpuParticleStream::PositionX) == 0);
    CHECK(static_cast<std::uint32_t>(GpuParticleStream::VelocityX) == 3);
    CHECK(static_cast<std::uint32_t>(GpuParticleStream::SizeX) == 6);
    CHECK(static_cast<std::uint32_t>(GpuParticleStream::Rotation) == 8);
    CHECK(static_cast<std::uint32_t>(GpuParticleStream::Age) == 10);
    CHECK(static_cast<std::uint32_t>(GpuParticleStream::PackedColor) == 12);
    CHECK(static_cast<std::uint32_t>(GpuParticleStream::CustomFlags) == 13);
    CHECK(static_cast<std::uint32_t>(GpuParticleStream::ParticleIdLow) == 14);
    CHECK(static_cast<std::uint32_t>(GpuParticleStream::ParticleIdHigh) == 15);
    CHECK(static_cast<std::uint32_t>(GpuParticleStream::Alive) == 16);
    CHECK(static_cast<std::uint32_t>(GpuParticleStream::SlotGeneration) == 17);
    CHECK(static_cast<std::uint32_t>(GpuParticleStream::FirstCustomFloat) == 18);
    const auto full = Layout(1'000'000, 16);
    CHECK(full.wordCount == 34'000'000);
    CHECK(full.byteCount == 136'000'000);
    std::uint32_t offset = 0;
    REQUIRE(TryGpuParticleWordOffset(full, 33, 999'999, offset));
    CHECK(offset == 33'999'999);
}

TEST_CASE("GPU particle layout admits exact caller and observed device byte ceilings", "[unit][renderer][vfx][gpu-layout]") {
    const auto exact = BuildGpuParticleLayout(5, 2, {640, 640});
    REQUIRE(exact.HasValue());
    for (const auto limits : {GpuParticleLayoutLimits{639, 640}, GpuParticleLayoutLimits{640, 639}}) {
        const auto rejected = BuildGpuParticleLayout(5, 2, limits);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == GpuParticleLayoutErrors::CapacityExceeded.code.Value());
    }
    CHECK(BuildGpuParticleLayout(5, 2, {0, 640}).HasError());
    CHECK(BuildGpuParticleLayout(5, 2, {640, 0}).HasError());
}

TEST_CASE("GPU particle hard bounds reject overflow-shaped requests before arithmetic", "[unit][renderer][vfx][gpu-layout]") {
    constexpr GpuParticleLayoutLimits largest{std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::uint64_t>::max()};
    for (const auto capacity : {0U, 1'000'001U, std::numeric_limits<std::uint32_t>::max()}) {
        const auto rejected = BuildGpuParticleLayout(capacity, 0, largest);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == GpuParticleLayoutErrors::InvalidLayout.code.Value());
    }
    CHECK(BuildGpuParticleLayout(1, 17, largest).HasError());
    CHECK(BuildGpuParticleLayout(1, std::numeric_limits<std::uint32_t>::max(), largest).HasError());
    CHECK(BuildGpuParticleLayout(1'000'000, 16, largest).HasValue());
}

TEST_CASE("GPU particle address lookup rejects padding and malformed layout copies", "[unit][renderer][vfx][gpu-layout]") {
    const auto layout = Layout(5, 2);
    std::uint32_t offset = 99;
    CHECK_FALSE(TryGpuParticleWordOffset(layout, 0, 5, offset));
    CHECK(offset == 0);
    offset = 99;
    CHECK_FALSE(TryGpuParticleWordOffset(layout, 20, 0, offset));
    CHECK(offset == 0);
    std::array<GpuParticleLayout, 6> malformed;
    malformed.fill(layout);
    malformed[0].capacity = std::numeric_limits<std::uint32_t>::max();
    malformed[1].customFloatStreams = 17;
    malformed[2].streamStrideWords = std::numeric_limits<std::uint32_t>::max();
    malformed[3].streamCount = std::numeric_limits<std::uint32_t>::max();
    malformed[4].wordCount = std::numeric_limits<std::uint32_t>::max();
    malformed[5].byteCount = std::numeric_limits<std::size_t>::max();
    for (const auto &candidate : malformed) {
        CHECK_FALSE(candidate.IsValid());
        offset = 99;
        CHECK_FALSE(TryGpuParticleWordOffset(candidate, 0, 0, offset));
        CHECK(offset == 0);
    }
}

TEST_CASE("GPU force words preserve the shared descriptor order and exact float bits", "[unit][renderer][vfx][gpu-layout]") {
    const std::array<Vfx::CpuParticleForceModule, 2> forces{{{.kind = Vfx::CpuParticleForceKind::Noise,
                                                              .vector = {1, -2, -0.0F},
                                                              .center = {std::numeric_limits<float>::denorm_min(), 0.5F, -8},
                                                              .strength = -3,
                                                              .falloff = 0.25F,
                                                              .frequency = 2,
                                                              .randomChannel = 42},
                                                             {.kind = Vfx::CpuParticleForceKind::Attraction,
                                                              .vector = {-0.0F, 3, 4},
                                                              .center = {5, 6, 7},
                                                              .strength = 0.5F,
                                                              .falloff = -0.0F,
                                                              .frequency = 0,
                                                              .randomChannel = 255}}};
    REQUIRE(Vfx::ValidateParticleForceModules(forces));
    std::array<std::uint32_t, 25> words;
    words.fill(0xDEADBEEFU);
    const auto result = PackGpuParticleForces(forces, words);
    REQUIRE(result.HasValue());
    CHECK(result.Value() == 24);
    const std::array<std::uint32_t, 12> expected{static_cast<std::uint32_t>(Vfx::CpuParticleForceKind::Noise),
                                                 42,
                                                 std::bit_cast<std::uint32_t>(-3.0F),
                                                 std::bit_cast<std::uint32_t>(0.25F),
                                                 std::bit_cast<std::uint32_t>(1.0F),
                                                 std::bit_cast<std::uint32_t>(-2.0F),
                                                 0x80000000U,
                                                 std::bit_cast<std::uint32_t>(2.0F),
                                                 1U,
                                                 std::bit_cast<std::uint32_t>(0.5F),
                                                 std::bit_cast<std::uint32_t>(-8.0F),
                                                 0U};
    CHECK(std::ranges::equal(std::span{words}.first(12), expected));
    CHECK(words[12] == static_cast<std::uint32_t>(Vfx::CpuParticleForceKind::Attraction));
    CHECK(words[13] == 255);
    CHECK(words[15] == 0x80000000U);
    CHECK(words[23] == 0);
    CHECK(words[24] == 0xDEADBEEFU);
    CHECK(GpuParticleForceWords::Bytes == 48);
}

TEST_CASE("GPU force lowering validates all inputs before modifying any output", "[unit][renderer][vfx][gpu-layout]") {
    std::array<Vfx::CpuParticleForceModule, 1> forces{};
    RequireForceFailure(forces, 11, GpuParticleLayoutErrors::CapacityExceeded);
    forces[0].kind = static_cast<Vfx::CpuParticleForceKind>(255);
    RequireForceFailure(forces, 12, GpuParticleLayoutErrors::InvalidForces);
    forces[0] = {};
    forces[0].center.x = std::numeric_limits<float>::quiet_NaN();
    RequireForceFailure(forces, 12, GpuParticleLayoutErrors::InvalidForces);
    forces[0] = {};
    forces[0].strength = std::numeric_limits<float>::infinity();
    RequireForceFailure(forces, 12, GpuParticleLayoutErrors::InvalidForces);
    forces[0] = {};
    forces[0].falloff = -1;
    RequireForceFailure(forces, 12, GpuParticleLayoutErrors::InvalidForces);
    std::array<Vfx::CpuParticleForceModule, 65> overCapacity{};
    RequireForceFailure(overCapacity, 768, GpuParticleLayoutErrors::InvalidForces);
    std::array<Vfx::CpuParticleForceModule, 2> duplicateNoise{
        {{.kind = Vfx::CpuParticleForceKind::Noise}, {.kind = Vfx::CpuParticleForceKind::Noise}}};
    RequireForceFailure(duplicateNoise, 24, GpuParticleLayoutErrors::InvalidForces);
}

TEST_CASE("GPU force lowering rejects descriptor aliases without modifying their representation", "[unit][renderer][vfx][gpu-layout]") {
    std::array<Vfx::CpuParticleForceModule, 2> forces{};
    const auto bytes = std::as_bytes(std::span{forces});
    std::array<std::byte, sizeof(forces)> before;
    std::ranges::copy(bytes, before.begin());
    static_assert(alignof(Vfx::CpuParticleForceModule) >= alignof(std::uint32_t));
    static_assert(sizeof(forces) >= GpuParticleForceWords::Bytes);
    const auto output = std::span{reinterpret_cast<std::uint32_t *>(forces.data()), GpuParticleForceWords::Count};
    const auto result = PackGpuParticleForces(std::span{forces}.first(1), output);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == GpuParticleLayoutErrors::InputAliased.code.Value());
    CHECK(std::ranges::equal(bytes, before));
}

TEST_CASE("Empty GPU force stacks write no prefix or padding", "[unit][renderer][vfx][gpu-layout]") {
    std::array<std::uint32_t, 2> words{0x01234567, 0x89ABCDEF};
    const auto result = PackGpuParticleForces({}, words);
    REQUIRE(result.HasValue());
    CHECK(result.Value() == 0);
    CHECK(words[0] == 0x01234567);
    CHECK(words[1] == 0x89ABCDEF);
}
