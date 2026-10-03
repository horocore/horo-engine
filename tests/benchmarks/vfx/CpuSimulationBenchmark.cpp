#include "support/VfxCpuQualificationFixtures.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
    using namespace Horo::Vfx;
    using namespace Horo::Vfx::Tests;
    constexpr std::uint64_t WarmupTicks = 30;
    constexpr std::uint64_t MeasuredTicks = 240;
    constexpr std::array<std::uint64_t, 3> Checkpoints{0, 59, 119};
    constexpr std::array<double, 3> ReferenceP99BudgetMilliseconds{0.02, 0.10, 0.50};

    void PrintParticle(const CpuQualificationParticle &particle) {
        std::cout << "{\"id\":" << particle.id << ",\"position\":[" << particle.position[0] << ',' << particle.position[1] << ','
                  << particle.position[2] << "],\"velocity\":[" << particle.velocity[0] << ',' << particle.velocity[1] << ','
                  << particle.velocity[2] << "],\"packedColor\":" << particle.packedColor << '}';
    }

    void PrintSnapshot(const CpuQualificationSnapshot &snapshot) {
        const auto &step = snapshot.step;
        std::cout << "{\"tick\":" << snapshot.tick << ",\"requestedBirths\":" << step.requestedBirths << ",\"spawned\":" << step.spawned
                  << ",\"dropped\":" << step.dropped << ",\"killed\":" << step.killed << ",\"collisions\":" << step.collisions
                  << ",\"active\":" << step.active << ",\"committedGeneration\":" << step.committedGeneration
                  << ",\"identityHash\":" << snapshot.identityHash << ",\"colorHash\":" << snapshot.colorHash << ",\"particles\":[";
        for (std::size_t index = 0; index < snapshot.particles.size(); ++index) {
            if (index != 0)
                std::cout << ',';
            PrintParticle(snapshot.particles[index]);
        }
        std::cout << "]}";
    }

    void PrintBaseline() {
        std::cout << std::setprecision(9);
        std::cout << "{\"schemaVersion\":1,\"rngVersion\":" << CpuParticleSimulationHardLimits::RandomAlgorithmVersion
                  << ",\"deltaSeconds\":" << CpuQualificationDeltaSeconds
                  << ",\"numericTolerance\":{\"absolute\":0.002,\"relative\":0.0002},\"scenes\":[";
        for (std::size_t sceneIndex = 0; sceneIndex < CpuQualificationWorkloads.size(); ++sceneIndex) {
            const auto &workload = CpuQualificationWorkloads[sceneIndex];
            auto simulator = CpuQualificationSimulator(workload, static_cast<std::uint32_t>(sceneIndex + 1));
            if (sceneIndex != 0)
                std::cout << ',';
            std::cout << "{\"name\":\"" << workload.name << "\",\"particles\":" << workload.particles << ",\"checkpoints\":[";
            std::size_t checkpoint = 0;
            for (std::uint64_t tick = 0; tick <= Checkpoints.back(); ++tick) {
                const auto step = simulator.Advance(
                    {.deltaSeconds = CpuQualificationDeltaSeconds, .burstCount = tick == 0 ? workload.particles : 0U, .tick = tick});
                if (step.HasError())
                    throw std::runtime_error("CPU baseline replay failed");
                if (checkpoint < Checkpoints.size() && tick == Checkpoints[checkpoint]) {
                    if (checkpoint != 0)
                        std::cout << ',';
                    PrintSnapshot(CpuQualificationCapture(simulator, tick, step.Value()));
                    ++checkpoint;
                }
            }
            std::cout << "]}";
        }
        std::cout << "]}\n";
    }

    void PrintTimings() {
        std::cout << "scene,particles,delta_seconds,warmup_ticks,measured_ticks,mean_ms,p99_ms,max_ms,budget_p99_ms,checksum\n";
        bool withinBudget = true;
        for (std::size_t sceneIndex = 0; sceneIndex < CpuQualificationWorkloads.size(); ++sceneIndex) {
            const auto &workload = CpuQualificationWorkloads[sceneIndex];
            auto simulator = CpuQualificationSimulator(workload, static_cast<std::uint32_t>(sceneIndex + 1));
            std::vector<double> elapsed;
            elapsed.reserve(MeasuredTicks);
            std::uint64_t checksum{};
            for (std::uint64_t tick = 0; tick < WarmupTicks + MeasuredTicks; ++tick) {
                const auto start = std::chrono::steady_clock::now();
                const auto step = simulator.Advance(
                    {.deltaSeconds = CpuQualificationDeltaSeconds, .burstCount = tick == 0 ? workload.particles : 0U, .tick = tick});
                const auto end = std::chrono::steady_clock::now();
                if (step.HasError())
                    throw std::runtime_error("CPU benchmark step failed");
                checksum += step.Value().active + step.Value().collisions;
                if (tick >= WarmupTicks)
                    elapsed.push_back(std::chrono::duration<double, std::milli>(end - start).count());
            }
            double sum{};
            for (const double sample : elapsed)
                sum += sample;
            std::ranges::sort(elapsed);
            const double p99 = elapsed[MeasuredTicks * 99 / 100];
            withinBudget = withinBudget && p99 <= ReferenceP99BudgetMilliseconds[sceneIndex];
            std::cout << workload.name << ',' << workload.particles << ',' << CpuQualificationDeltaSeconds << ',' << WarmupTicks << ','
                      << MeasuredTicks << ',' << std::fixed << std::setprecision(6) << sum / MeasuredTicks << ',' << p99 << ','
                      << elapsed.back() << ',' << ReferenceP99BudgetMilliseconds[sceneIndex] << ',' << checksum << '\n';
        }
        if (!withinBudget)
            throw std::runtime_error("CPU reference scene exceeds its p99 budget");
    }
}  // namespace

int main(const int argc, const char *const argv[]) {
    try {
        if (argc == 2 && std::string_view{argv[1]} == "--baseline")
            PrintBaseline();
        else if (argc == 1)
            PrintTimings();
        else
            return 2;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
