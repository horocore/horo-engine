#include "StreamingPlannerFixture.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
    using namespace Horo::WorldStreaming;
    using namespace PlannerFixture;
    using Clock = std::chrono::steady_clock;

    double Milliseconds(const Clock::time_point begin) {
        return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    }

    struct SpatialMeasurement {
        double oldMilliseconds{}, spatialMilliseconds{}, maximumSpatialQuantum{};
        std::size_t matches{}, spatialQuanta{}, nodes{}, candidates{}, originalNodes{}, originalCandidates{};
    };

    SpatialMeasurement MeasureSpatial(const WorldPartitionRegistrySnapshot &partition, const WorldPartitionSpatialQuery &query) {
        std::vector<WorldPartitionCellHandle> baseline(partition.Cells().size());
        auto begin = Clock::now();
        const auto original = partition.Query(query, baseline).Value();
        const auto oldMilliseconds = Milliseconds(begin);
        auto cursor = partition.BeginQuery(query).Value();
        std::array<WorldPartitionCellHandle, 128> batch;
        std::size_t matches = 0, spatialQuanta = 0, nodes = 0, candidates = 0;
        double maximumSpatialQuantum = 0;
        begin = Clock::now();
        while (!cursor.Complete()) {
            const auto quantumBegin = Clock::now();
            const auto result = partition.QueryNext(cursor, {128, 128}, batch).Value();
            maximumSpatialQuantum = std::max(maximumSpatialQuantum, Milliseconds(quantumBegin));
            if (result.nodesVisited > 128 || result.candidatesExamined > 128)
                throw std::runtime_error("spatial budget exceeded");
            matches += result.matches;
            nodes += result.nodesVisited;
            candidates += result.candidatesExamined;
            ++spatialQuanta;
        }
        const auto spatialMilliseconds = Milliseconds(begin);
        if (matches != original.matches)
            throw std::runtime_error("old/new spatial result disagreement");
        return {oldMilliseconds, spatialMilliseconds,   maximumSpatialQuantum,      matches, spatialQuanta, nodes,
                candidates,      original.nodesVisited, original.candidatesExamined};
    }

    void Measure(const std::string_view workload, const std::int32_t spacing, const std::int32_t side) {
        const auto partition = Partition(side, spacing);
        const WorldPartitionSpatialQuery query{{Point(0), Point(side * 100 - 1, side * 100 - 1, 99)}, {}, {}};
        const auto spatial = MeasureSpatial(partition, query);
        auto config = Config(static_cast<std::uint32_t>(partition.Cells().size()));
        config.limits.nodesPerFrame = 128;
        config.limits.candidatesPerFrame = 128;
        config.limits.stabilityRowsPerFrame = 128;
        auto planner = StreamingSourcePlanner::Create(config, partition, Environment()).Value();
        auto sourceRequest = Source();
        sourceRequest.shape = StreamingSourceBox{query.bounds.minimum, query.bounds.maximum};
        auto owner = planner->Register(sourceRequest).Value();
        std::size_t serviceQuanta = 0, maximumNodes = 0, maximumCandidates = 0, maximumStability = 0;
        double maximumPlannerQuantum = 0;
        const auto begin = Clock::now();
        while (true) {
            const auto quantumBegin = Clock::now();
            const auto result = planner->Service(1'000).Value();
            maximumPlannerQuantum = std::max(maximumPlannerQuantum, Milliseconds(quantumBegin));
            maximumNodes = std::max(maximumNodes, result.nodesVisited);
            maximumCandidates = std::max(maximumCandidates, result.candidatesExamined);
            maximumStability = std::max(maximumStability, result.stabilityRows);
            ++serviceQuanta;
            if (result.published)
                break;
        }
        const auto plannerMilliseconds = Milliseconds(begin);
        const auto plan = planner->Plan().Value();
        if (plan.cells.size() != spatial.matches || maximumNodes > 128 || maximumCandidates > 128 || maximumStability > 128)
            throw std::runtime_error("integrated result/budget disagreement");
        std::cout << "{\"workload\":\"" << workload << "\",\"manifestCells\":" << partition.Cells().size()
                  << ",\"matches\":" << spatial.matches << ",\"oldQueryMs\":" << spatial.oldMilliseconds
                  << ",\"oldReportedNodesPerPass\":" << spatial.originalNodes
                  << ",\"oldReportedCandidatesPerPass\":" << spatial.originalCandidates
                  << ",\"boundedSpatialTotalMs\":" << spatial.spatialMilliseconds
                  << ",\"boundedSpatialMaxQuantumMs\":" << spatial.maximumSpatialQuantum
                  << ",\"boundedSpatialQuanta\":" << spatial.spatialQuanta << ",\"boundedSpatialNodes\":" << spatial.nodes
                  << ",\"boundedSpatialCandidates\":" << spatial.candidates << ",\"integratedTotalMs\":" << plannerMilliseconds
                  << ",\"integratedMaxQuantumMs\":" << maximumPlannerQuantum << ",\"integratedQuanta\":" << serviceQuanta
                  << ",\"maximumNodes\":" << maximumNodes << ",\"maximumCandidates\":" << maximumCandidates
                  << ",\"maximumStabilityRows\":" << maximumStability << "}\n";
    }
}  // namespace

int main(int argc, char **argv) {
    try {
        const auto side = argc > 1 ? std::atoi(argv[1]) : 256;
        if (side < 16 || side > 256)
            return 2;
        std::cout << "{\"buildMode\":\"" << HORO_BENCHMARK_BUILD_MODE << "\",\"compiler\":\"";
#if defined(_MSC_VER)
        std::cout << "MSVC " << _MSC_FULL_VER;
#else
        std::cout << __VERSION__;
#endif
        std::cout << "\",\"platform\":\"";
#if defined(_WIN32)
        std::cout << "Windows";
#elif defined(__APPLE__)
        std::cout << "macOS";
#elif defined(__linux__)
        std::cout << "Linux";
#else
        std::cout << "other";
#endif
        std::cout << "\",\"architecture\":\"";
#if defined(__aarch64__) || defined(_M_ARM64)
        std::cout << "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
        std::cout << "x86_64";
#else
        std::cout << "other";
#endif
        std::cout << "\"}\n";
        Measure("sparse", 16, side);
        Measure("dense", 1, side);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
