#include "Horo/AI/AIErrors.h"
#include "Horo/AI/PerceptionSenseProvider.h"
#include "Horo/Gameplay/GameModule.h"

#include <array>
#include <iostream>
#include <limits>

extern "C" const Horo::Gameplay::GeneratedGameplayDescriptorBundle *GetGameplayDescriptorBundle() noexcept;

namespace {
    using namespace Horo;
    using namespace Horo::AI;

    /** @brief Admits the generated sense with the same listener contract required by a project host. */
    std::unique_ptr<PerceptionSenseInstance> CreateSense(const PerceptionSenseRegistration &registration,
                                                         const PerceptionSenseBinding &binding) {
        const auto generation = PerceptionSenseGeneration::Capture(std::span{&registration, 1}, std::make_shared<std::atomic_bool>(true));
        if (generation.HasError())
            return {};
        const auto &contract = registration.contract;
        PerceptionListenerDescriptor listener;
        listener.identity = binding.listenerType;
        listener.origin = contract.sense.origin;
        listener.displayName = "Guard proximity listener";
        listener.sense = contract.sense.identity;
        listener.stimuli[0] = {contract.stimulus.identity, 1, 1};
        listener.stimulusCount = 1;
        const auto capabilities = PerceptionCapabilitySet::Of(PerceptionCapability::SpatialCandidateQuery);
        const auto registry = PerceptionDescriptorRegistry::Capture({std::span{&contract.sense, 1}, std::span{&contract.stimulus, 1},
                                                                     std::span{&listener, 1}},
                                                                    capabilities);
        if (registry.HasError())
            return {};
        auto created = generation.Value()->Create(generation.Value(), contract.sense.identity, registry.Value(), capabilities, binding);
        return created.HasValue() ? std::move(created).Value() : nullptr;
    }

    /** @brief Checks range, stable candidate order, the empty slice and extreme signed distances. */
    bool CheckSense(PerceptionSenseInstance &sense, const PerceptionSenseBinding &binding) {
        using Math::WorldCoordinate64;
        constexpr auto east = std::numeric_limits<std::int64_t>::max();
        const std::array candidates{PerceptionSenseCandidate{{7, 1, 1}, WorldCoordinate64::FromMillimeters(east, 0, 0)},
                                    PerceptionSenseCandidate{{7, 2, 1}, WorldCoordinate64::FromMillimeters(east, 1, 0)},
                                    PerceptionSenseCandidate{{7, 3, 1},
                                                             WorldCoordinate64::FromMillimeters(std::numeric_limits<std::int64_t>::min(), 0,
                                                                                                0)}};
        const auto position = WorldCoordinate64::FromMillimeters(east - 3'000, 0, 0);
        AiWorkBudget budget{128};
        const auto evaluated = sense.Evaluate({binding.agent, binding.listener, position, 1, 1, candidates}, budget, budget);
        if (evaluated.HasError() || evaluated.Value().count != candidates.size())
            return false;
        constexpr std::array expected{true, false, false};
        for (std::size_t index = 0; index < expected.size(); ++index) {
            if (evaluated.Value().samples[index].candidate != index || evaluated.Value().samples[index].sensed != expected[index])
                return false;
        }
        const auto empty = sense.Evaluate({binding.agent, binding.listener, position, 2, 1, {}}, budget, budget);
        return empty.HasValue() && empty.Value().count == 0;
    }

    /** @brief Confirms rejected host slices leave the next fixed tick available. */
    bool CheckAdmission(PerceptionSenseInstance &sense, const PerceptionSenseBinding &binding) {
        constexpr std::size_t maximumCandidates = 32;
        const auto position = Math::WorldCoordinate64::FromMillimeters(0, 0, 0);
        std::array<PerceptionSenseCandidate, maximumCandidates + 1> candidates{};
        for (std::uint32_t index = 0; index < candidates.size(); ++index)
            candidates[index] = {{7, index + 1, 1}, position};

        AiWorkBudget available{128};
        const auto oversized = sense.Evaluate({binding.agent, binding.listener, position, 3, 1, candidates}, available, available);
        if (oversized.HasValue() || oversized.ErrorValue().code.Value() != AIErrors::PerceptionSpatialLimitExceeded.code.Value() ||
            available.Remaining() != 128)
            return false;

        const auto allowed = std::span{candidates}.first(maximumCandidates);
        AiWorkBudget depleted{1};
        const auto deferred = sense.Evaluate({binding.agent, binding.listener, position, 3, 1, allowed}, depleted, available);
        if (deferred.HasValue() || deferred.ErrorValue().code.Value() != AIErrors::SchedulerBudgetExhausted.code.Value())
            return false;

        const auto retried = sense.Evaluate({binding.agent, binding.listener, position, 3, 1, allowed}, available, available);
        if (retried.HasError() || retried.Value().count != maximumCandidates)
            return false;
        for (std::size_t index = 0; index < maximumCandidates; ++index)
            if (retried.Value().samples[index].candidate != index || !retried.Value().samples[index].sensed)
                return false;
        return true;
    }
}  // namespace

int main() {
    const auto *bundle = GetGameplayDescriptorBundle();
    if (!bundle || bundle->diagnosticCount != 0 || bundle->perceptionSenseCount != 1)
        return 1;
    const PerceptionSenseBinding binding{{AiRuntimeIncarnation::Create(7).Value(), {0, 1}},
                                         {7, 0, 1},
                                         PerceptionListenerTypeId::Create(83012).Value()};
    const auto sense = CreateSense(bundle->perceptionSenses[0], binding);
    if (!sense || !CheckSense(*sense, binding) || !CheckAdmission(*sense, binding)) {
        std::cerr << "Guard proximity failed its range, capacity, or budget check.\n";
        return 1;
    }
    return 0;
}
