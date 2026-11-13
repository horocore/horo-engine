#include "Horo/Vfx/ParticleSimulationData.h"

#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Vfx;

    struct Golden {
        ParticleRandomKey key;
        ParticleRandomSample sample;
        std::uint64_t word;
    };

    // Fixed values independently transcribed from the PRE-extraction full-simulation algorithm.
    // Reference CpuParticleSimulatorInternal.h SHA256 3270257a4d0e6e9af201b8e564951175d0dad205bf10576e815f80fcde883c6a.
    constexpr std::array
        Goldens{Golden{{0x0ULL, {0x1ULL, 2u, 3u}, {0x4ULL, 5u, 6u}}, {0x1ULL, 10u, 0u, 0x0ULL}, 0xa735926dfcc74978ULL},
                Golden{{0xffffffffffffffffULL, {0xffffffffffffffffULL, 4294967295u, 4294967295u}, {0x8000000000000000ULL, 1u, 2u}},
                       {0xffffffffffffffffULL, 255u, 4294967295u, 0xffffffffffffffffULL},
                       0x367dbcb5aba44948ULL},
                Golden{{0x0ULL, {0x100000001ULL, 2u, 3u}, {0x4ULL, 5u, 6u}}, {0x1ULL, 10u, 0u, 0x0ULL}, 0xb69d1e79814030d6ULL},
                Golden{{0x0ULL, {0x4ULL, 5u, 6u}, {0x1ULL, 2u, 3u}}, {0x1ULL, 10u, 0u, 0x0ULL}, 0x1261fea87202e4ULL},
                Golden{{0x0ULL, {0x1ULL, 2u, 3u}, {0x4ULL, 5u, 6u}}, {0x1ULL, 11u, 0u, 0x0ULL}, 0x427e16524526bc1cULL},
                Golden{{0x0ULL, {0x1ULL, 2u, 3u}, {0x4ULL, 5u, 6u}}, {0x1ULL, 10u, 1u, 0x0ULL}, 0x17984258fc81fd7dULL},
                Golden{{0x0ULL, {0x1ULL, 2u, 3u}, {0x4ULL, 5u, 6u}}, {0x1ULL, 10u, 0u, 0x1ULL}, 0xbadec39c9dfc9253ULL},
                Golden{{0x0ULL, {0x1ULL, 3u, 2u}, {0x4ULL, 5u, 6u}}, {0x1ULL, 10u, 0u, 0x0ULL}, 0xe397c7f8ab80d2f8ULL}};
}  // namespace

TEST_CASE("Shared particle random words preserve pre-extraction semantic-key golden vectors", "[vfx][simulation-data]") {
    for (const auto &golden : Goldens)
        CHECK(ComputeParticleRandomWord(golden.key, golden.sample) == golden.word);
    CHECK(ParticleSimulationDataLimits::RandomAlgorithmVersion == 1);
}

TEST_CASE("Shared particle identity bases retain pre-extraction masked key goldens", "[vfx][simulation-data]") {
    CHECK(ComputeParticleIdentityBase(Goldens[0].key) == 0x8b3d9537001bULL);
    CHECK(ComputeParticleIdentityBase(Goldens[1].key) == 0x53c701899221ULL);
}

TEST_CASE("Shared particle random conversion uses only the exact top twenty four bits", "[vfx][simulation-data]") {
    CHECK(std::bit_cast<std::uint32_t>(ParticleRandomUnitFloat(0)) == 0);
    CHECK(std::bit_cast<std::uint32_t>(ParticleRandomUnitFloat(0x000000ffffffffffULL)) == 0);
    CHECK(std::bit_cast<std::uint32_t>(ParticleRandomUnitFloat(std::numeric_limits<std::uint64_t>::max())) == 0x3f7fffff);
    CHECK(ParticleRandomUnitFloat(0x8000000000000000ULL) == 0.5F);
    CHECK(ParticleRandomUnitFloat(0x800000ffffffffffULL) == 0.5F);
}

TEST_CASE("Shared force validation admits the exact ordered stack and channel limits", "[vfx][simulation-data]") {
    std::array<CpuParticleForceModule, 65> forces;
    for (std::size_t index = 0; index < forces.size(); ++index)
        forces[index] = {.kind = CpuParticleForceKind::Noise, .randomChannel = static_cast<std::uint8_t>(10 + index)};
    CHECK(ValidateParticleForceModules({}));
    CHECK(ValidateParticleForceModules(std::span{forces}.first(64)));
    CHECK_FALSE(ValidateParticleForceModules(forces));
    forces[1].randomChannel = forces[0].randomChannel;
    CHECK_FALSE(ValidateParticleForceModules(std::span{forces}.first(2)));
}

TEST_CASE("Shared force descriptors preserve finite product and reserved-channel rejection", "[vfx][simulation-data]") {
    std::array<CpuParticleForceModule, 1> forces{};
    auto &force = forces[0];
    SECTION("unknown force") {
        force.kind = CpuParticleForceKind::Count;
    }
    SECTION("nonfinite vector") {
        force.vector.x = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("nonfinite center") {
        force.center.z = std::numeric_limits<float>::infinity();
    }
    SECTION("nonfinite strength") {
        force.strength = std::numeric_limits<float>::infinity();
    }
    SECTION("negative falloff") {
        force.falloff = -1;
    }
    SECTION("negative frequency") {
        force.frequency = -1;
    }
    SECTION("constant acceleration overflow") {
        force.vector.x = std::numeric_limits<float>::max();
        force.strength = 2;
    }
    SECTION("noise amplitude overflow") {
        force.kind = CpuParticleForceKind::Noise;
        force.strength = std::numeric_limits<float>::max();
        force.frequency = 2;
    }
    SECTION("reserved noise channel") {
        force.kind = CpuParticleForceKind::Noise;
        force.randomChannel = 9;
    }
    CHECK_FALSE(ValidateParticleForceModules(forces));
}

TEST_CASE("Shared force descriptors retain zero and negative-strength semantics", "[vfx][simulation-data]") {
    std::array<CpuParticleForceModule, 1> forces{};
    auto &force = forces[0];
    SECTION("non-noise channel remains unrestricted") {
        force.randomChannel = 0;
    }
    SECTION("negative constant strength") {
        force.kind = CpuParticleForceKind::Wind;
        force.vector = {1, 2, 3};
        force.strength = -2;
    }
    SECTION("zero noise frequency") {
        force.kind = CpuParticleForceKind::Noise;
        force.frequency = 0;
        force.randomChannel = 10;
    }
    SECTION("zero noise strength") {
        force.kind = CpuParticleForceKind::Noise;
        force.strength = 0;
        force.randomChannel = 255;
    }
    SECTION("attraction center is data rather than admission failure") {
        force.kind = CpuParticleForceKind::Attraction;
        force.center = {};
        force.strength = -1;
    }
    CHECK(ValidateParticleForceModules(forces));
}
