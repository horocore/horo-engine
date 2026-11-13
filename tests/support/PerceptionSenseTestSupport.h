#pragma once

#include "../unit/runtime/ai/AiTestSupport.h"
#include "Horo/AI/PerceptionSenseProvider.h"

namespace Horo::AI::PerceptionSenseTestSupport {
    /** @brief Value-only inputs for native callback, budget and executable-lifetime qualification. */
    struct InputFixture {
        CancellationSource cancellation;
        PerceptionCapabilitySet capabilities{PerceptionCapabilitySet::Of(PerceptionCapability::SpatialCandidateQuery)};
        PerceptionSenseBinding binding{{AiRuntimeIncarnation::Create(7).Value(), {0, 1}},
                                       {77, 0, 1},
                                       TestSupport::MakeIdentity<PerceptionListenerTypeId>(40),
                                       cancellation.Token()};
        std::array<PerceptionSenseCandidate, 2> candidates{{{{77, 1, 1}, Math::WorldCoordinate64::FromMillimeters(1100, 0, 0)},
                                                            {{77, 2, 1}, Math::WorldCoordinate64::FromMillimeters(2000, 0, 0)}}};

        /** @brief Captures the exact real generation metadata selected by the test. */
        PerceptionDescriptorRegistry Registry(const PerceptionSenseContract &contract) const {
            PerceptionListenerDescriptor listener;
            listener.identity = binding.listenerType;
            listener.origin = contract.sense.origin;
            listener.displayName = "custom listener";
            listener.sense = contract.sense.identity;
            listener.stimuli[0] = {contract.stimulus.identity, 1, 1};
            listener.stimulusCount = 1;
            auto captured = PerceptionDescriptorRegistry::Capture({std::span{&contract.sense, 1}, std::span{&contract.stimulus, 1},
                                                                   std::span{&listener, 1}},
                                                                  capabilities);
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }

        /** @brief Creates only through the actual retained generation's public admission path. */
        std::unique_ptr<PerceptionSenseInstance> Instance(const std::shared_ptr<const PerceptionSenseGeneration> &generation) const {
            REQUIRE(generation->Contracts().size() == 1);
            const auto &contract = generation->Contracts()[0];
            auto registry = Registry(contract);
            auto created = generation->Create(generation, contract.sense.identity, registry, capabilities, binding);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        /** @brief Returns a borrowed fixed publication for this invocation. */
        PerceptionSenseInput Input(const std::uint64_t tick = 1) const {
            return {binding.agent, binding.listener, Math::WorldCoordinate64::FromMillimeters(1000, 0, 0), tick, 1, candidates, {}};
        }
    };
}  // namespace Horo::AI::PerceptionSenseTestSupport
