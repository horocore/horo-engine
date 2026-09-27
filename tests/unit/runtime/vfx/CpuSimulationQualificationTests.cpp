#include "support/VfxCpuQualificationFixtures.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace Horo::Vfx::Tests {
    namespace {
        using Json = nlohmann::json;

        [[nodiscard]] bool Near(const float actual, const float expected, const double absolute, const double relative) {
            return std::fabs(static_cast<double>(actual) - expected) <=
                   absolute + relative * std::max(std::fabs(static_cast<double>(actual)), std::fabs(static_cast<double>(expected)));
        }

        void CheckBaseline(const CpuQualificationSnapshot &actual, const Json &expected, const double absolute, const double relative) {
            CHECK(actual.tick == expected.at("tick").get<std::uint64_t>());
            CHECK(actual.step.requestedBirths == expected.at("requestedBirths").get<std::uint64_t>());
            CHECK(actual.step.spawned == expected.at("spawned").get<std::uint32_t>());
            CHECK(actual.step.dropped == expected.at("dropped").get<std::uint64_t>());
            CHECK(actual.step.killed == expected.at("killed").get<std::uint32_t>());
            CHECK(actual.step.collisions == expected.at("collisions").get<std::uint32_t>());
            CHECK(actual.step.active == expected.at("active").get<std::uint32_t>());
            CHECK(actual.step.committedGeneration == expected.at("committedGeneration").get<std::uint64_t>());
            CHECK(actual.identityHash == expected.at("identityHash").get<std::uint64_t>());
            CHECK(actual.colorHash == expected.at("colorHash").get<std::uint64_t>());
            const auto &particles = expected.at("particles");
            REQUIRE(particles.size() == actual.particles.size());
            for (std::size_t sample = 0; sample < actual.particles.size(); ++sample) {
                const auto &particle = actual.particles[sample];
                const auto &reference = particles.at(sample);
                CHECK(particle.id == reference.at("id").get<std::uint64_t>());
                CHECK(particle.packedColor == reference.at("packedColor").get<std::uint32_t>());
                for (std::size_t component = 0; component < 3; ++component) {
                    CHECK(Near(particle.position[component], reference.at("position").at(component).get<float>(), absolute, relative));
                    CHECK(Near(particle.velocity[component], reference.at("velocity").at(component).get<float>(), absolute, relative));
                }
            }
        }

        void CheckSameExecutableReplay(const CpuQualificationSnapshot &first, const CpuQualificationSnapshot &second) {
            CHECK(first.tick == second.tick);
            CHECK(first.step == second.step);
            CHECK(first.identityHash == second.identityHash);
            CHECK(first.colorHash == second.colorHash);
            for (std::size_t sample = 0; sample < first.particles.size(); ++sample) {
                CHECK(first.particles[sample].id == second.particles[sample].id);
                CHECK(first.particles[sample].packedColor == second.particles[sample].packedColor);
                CHECK(first.particles[sample].position == second.particles[sample].position);
                CHECK(first.particles[sample].velocity == second.particles[sample].velocity);
            }
        }
    }  // namespace

    TEST_CASE("CPU simulation replay matches published cross-platform reference scenes", "[unit][vfx][qualification][replay]") {
        std::ifstream input{HORO_VFX_CPU_BASELINE_PATH};
        REQUIRE(input.is_open());
        const Json baseline = Json::parse(input);
        REQUIRE(baseline.at("schemaVersion") == 1);
        REQUIRE(baseline.at("rngVersion") == CpuParticleSimulationHardLimits::RandomAlgorithmVersion);
        REQUIRE(baseline.at("deltaSeconds").get<float>() == CpuQualificationDeltaSeconds);
        const double absolute = baseline.at("numericTolerance").at("absolute").get<double>();
        const double relative = baseline.at("numericTolerance").at("relative").get<double>();
        REQUIRE(absolute > 0.0);
        REQUIRE(relative > 0.0);
        const auto &scenes = baseline.at("scenes");
        REQUIRE(scenes.size() == CpuQualificationWorkloads.size());
        for (std::size_t sceneIndex = 0; sceneIndex < CpuQualificationWorkloads.size(); ++sceneIndex) {
            const auto &workload = CpuQualificationWorkloads[sceneIndex];
            const auto &reference = scenes.at(sceneIndex);
            INFO(workload.name);
            REQUIRE(reference.at("name") == workload.name);
            REQUIRE(reference.at("particles") == workload.particles);
            const auto &checkpoints = reference.at("checkpoints");
            REQUIRE(checkpoints.size() == 3);
            auto first = CpuQualificationSimulator(workload, static_cast<std::uint32_t>(sceneIndex * 2 + 1));
            auto second = CpuQualificationSimulator(workload, static_cast<std::uint32_t>(sceneIndex * 2 + 2));
            std::size_t checkpoint = 0;
            for (std::uint64_t tick = 0; tick <= 119; ++tick) {
                const CpuParticleSimulationStep request{.deltaSeconds = CpuQualificationDeltaSeconds,
                                                        .burstCount = tick == 0 ? workload.particles : 0U,
                                                        .tick = tick};
                const auto left = first.Advance(request);
                const auto right = second.Advance(request);
                REQUIRE(left.HasValue());
                REQUIRE(right.HasValue());
                CHECK(left.Value() == right.Value());
                if (checkpoint < checkpoints.size() && tick == checkpoints.at(checkpoint).at("tick").get<std::uint64_t>()) {
                    const auto a = CpuQualificationCapture(first, tick, left.Value());
                    const auto b = CpuQualificationCapture(second, tick, right.Value());
                    CheckSameExecutableReplay(a, b);
                    CheckBaseline(a, checkpoints.at(checkpoint), absolute, relative);
                    ++checkpoint;
                }
            }
            CHECK(checkpoint == checkpoints.size());
        }
    }
}  // namespace Horo::Vfx::Tests
