#include "Horo/AI/DecisionGraphInstance.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Gameplay/GameModule.h"

#include <filesystem>
#include <fstream>
#include <iostream>

extern "C" const Horo::Gameplay::GeneratedGameplayDescriptorBundle *GetGameplayDescriptorBundle() noexcept;

namespace {
    using namespace Horo;
    using namespace Horo::AI;

    /** @brief Reads the bounded checked-in source transport for one asset. */
    std::vector<std::uint8_t> ReadAsset(const std::filesystem::path &path, const std::size_t maximumBytes) {
        std::ifstream input{path, std::ios::binary | std::ios::ate};
        const auto size = input.tellg();
        if (size <= 0 || static_cast<std::size_t>(size) > maximumBytes)
            return {};
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return input ? std::move(bytes) : std::vector<std::uint8_t>{};
    }

    /** @brief Compiles and reloads the authored plan using the same admitted inputs. */
    std::shared_ptr<const CookedDecisionPlan> LoadPlan(const std::filesystem::path &path,
                                                       const DecisionCompilationEnvironment &environment) {
        DecisionGraphAssetId root;
        const auto source = DecisionPlanCodec::DecodeSource(ReadAsset(path, DecisionCodecLimits{}.maximumBytes), root);
        if (source.HasError()) {
            std::cerr << source.ErrorValue().message << '\n';
            return {};
        }
        const auto compiled = DecisionGraphCompiler::Compile(root, source.Value(), environment);
        if (compiled.HasError()) {
            std::cerr << compiled.ErrorValue().message << '\n';
            return {};
        }
        if (!compiled.Value().plan) {
            for (const auto &diagnostic : compiled.Value().validation.Diagnostics())
                std::cerr << diagnostic.message << '\n';
            return {};
        }
        const auto cooked = DecisionPlanCodec::EncodeCooked(*compiled.Value().plan);
        if (cooked.HasError())
            return {};
        const auto restored = DecisionPlanCodec::DecodeCooked(cooked.Value(), environment);
        return restored.HasValue() ? restored.Value().plan : nullptr;
    }

    /** @brief Executes the authored tree with the generated module descriptor at its fixed tick boundaries. */
    bool CheckHold(const std::filesystem::path &assets, const std::shared_ptr<const DecisionNodeGeneration> &providers) {
        auto decoded = BlackboardSchemaSourceCodec::Decode(ReadAsset(assets / "Guard.horo_bb", BlackboardSchemaSourceCodec::MaximumBytes));
        if (decoded.HasError()) {
            std::cerr << decoded.ErrorValue().message << '\n';
            return false;
        }
        const auto schema = std::make_shared<const BlackboardSchema>(std::move(decoded).Value().schema);
        const auto plan = LoadPlan(assets / "HoldWaypoint.horo_bt", {.nodes = providers, .schemas = {schema}});
        if (!plan)
            return false;
        const AgentHandle agent{AiRuntimeIncarnation::Create(1).Value(), {0, 1}};
        const BlackboardInstanceBinding binding{agent, schema->Identity(), schema->Version(), 1, 1};
        const auto board = BlackboardInstance::Create(binding, schema);
        if (board.HasError())
            return false;
        const auto snapshot = board.Value()->Snapshot();
        CancellationSource cancellation;
        const AiTaskOperationContext operation{TaskId::Create(83005).Value(), {agent.incarnation, {0, 1}}, agent, cancellation.Token()};
        const auto graph = DecisionGraphInstance::Create(plan, providers,
                                                         {.blackboard = binding,
                                                          .operation = operation,
                                                          .firstTaskSlot = 1,
                                                          .availableTaskSlots = plan->Storage().taskSlots});
        if (snapshot.HasError() || graph.HasError())
            return false;
        for (std::uint64_t tick = 10; tick <= 40; ++tick) {
            const auto result = graph.Value()->Step({tick, binding, snapshot.Value(), {}});
            const auto expected = tick == 40 ? AiTaskState::Succeeded : AiTaskState::Running;
            if (result.HasError() || result.Value() != expected)
                return false;
        }
        return true;
    }
}  // namespace

int main(const int argc, const char *const *argv) {
    if (argc != 2)
        return 1;
    const auto *bundle = GetGameplayDescriptorBundle();
    if (!bundle || bundle->diagnosticCount != 0 || bundle->decisionNodeCount == 0) {
        std::cerr << "Expected a generated guard decision descriptor.\n";
        return 1;
    }
    const auto providers =
        DecisionNodeGeneration::Capture({bundle->decisionNodes, bundle->decisionNodeCount}, std::make_shared<std::atomic_bool>(true));
    if (providers.HasError() || !CheckHold(argv[1], providers.Value())) {
        std::cerr << "The authored tree did not wait for exactly 30 fixed ticks.\n";
        return 1;
    }
    return 0;
}
