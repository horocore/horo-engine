#pragma once

#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Navigation/NavigationRuntimeQueues.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace Horo::Navigation::TestSupport {
    /** @brief Minimal valid backend with optional destruction observation for runtime contract tests. */
    class ObservedNavigationBackend final : public INavigationQueryBackend {
    public:
        explicit ObservedNavigationBackend(std::shared_ptr<std::atomic<std::uint32_t>> destructions = {}) noexcept
            : destructions_(std::move(destructions)) {}

        ~ObservedNavigationBackend() override {
            if (destructions_)
                destructions_->fetch_add(1, std::memory_order_relaxed);
        }

        [[nodiscard]] NavigationProviderCapabilities Capabilities() const noexcept override {
            constexpr NavigationQueryLimits limits{
                .maximumNodeExpansions = 64,
                .maximumResultPoints = 16,
                .maximumSearchDistanceMeters = 100.0F,
            };
            return MakeAvailablePathQueryCapabilities(1, limits, 1);
        }

        [[nodiscard]] Result<NavigationPath> FindPath(const NavigationPathRequest &, const CancellationToken &) const override {
            return Result<NavigationPath>::Failure(MakeError(NavigationErrors::NoNavigationData));
        }

    private:
        std::shared_ptr<std::atomic<std::uint32_t>> destructions_;
    };

    [[nodiscard]] inline NavigationWorldId World(const std::uint64_t value = 7) {
        return NavigationWorldId::Create(value).Value();
    }

    [[nodiscard]] inline NavigationGeneration Topology(const std::uint64_t value = 9) {
        return NavigationGeneration::Create(value).Value();
    }

    [[nodiscard]] inline NavigationWorldActivationDescriptor Activation(const std::uint64_t scene = 11,
                                                                        const std::uint64_t sceneGeneration = 12,
                                                                        const std::uint64_t world = 7, const std::uint64_t topology = 9) {
        return {
            .scene = NavigationSceneRuntimeId::Create(scene).Value(),
            .sceneGeneration = NavigationSceneGeneration::Create(sceneGeneration).Value(),
            .world = World(world),
            .topology = Topology(topology),
        };
    }

    [[nodiscard]] inline NavigationPathRequest Request(const NavigationWorldId world = World(),
                                                       const NavigationGeneration topology = Topology()) {
        return {
            .world = world,
            .topology = topology,
            .start = {1.0F, 2.0F, 3.0F},
            .destination = {4.0F, 5.0F, 6.0F},
            .filter = NavigationFilterId::Create(1).Value(),
            .coveragePolicy = NavigationPathCoveragePolicy::RequireComplete,
            .requirement =
                {
                    .query = NavigationQueryKind::Path,
                    .quality = NavigationQualityLevel::Balanced,
                    .limits = {.maximumNodeExpansions = 64, .maximumResultPoints = 16, .maximumSearchDistanceMeters = 100.0F},
                },
        };
    }

    [[nodiscard]] inline NavigationRuntimeQueueDescriptor QueueDescriptor(const std::uint32_t slots = 8) {
        return {
            .commandSlots = slots,
            .querySlots = slots,
            .completionSlots = slots,
            .maximumOwnedBytes = std::numeric_limits<std::size_t>::max(),
        };
    }

    [[nodiscard]] inline NavRequestHandle RequestHandle(const NavigationWorldId world = World(), const std::uint32_t index = 3,
                                                        const std::uint32_t generation = 2) {
        return {.world = world, .slot = {.index = index, .generation = generation}};
    }

    [[nodiscard]] inline NavigationQueuedCompletion CancelledCompletion(const NavigationWorldActivationDescriptor &activation,
                                                                        const std::uint64_t sequence, const NavRequestHandle handle) {
        return {
            .acceptedSequence = sequence,
            .handle = handle,
            .scene = activation.scene,
            .sceneGeneration = activation.sceneGeneration,
            .world = activation.world,
            .topology = activation.topology,
            .outcome = NavigationCancelled{},
        };
    }

    [[nodiscard]] inline std::unique_ptr<INavigationQueryBackend> MakeObservedNavigationBackend(
        const std::shared_ptr<std::atomic<std::uint32_t>> &destructions = {}) {
        return std::make_unique<ObservedNavigationBackend>(destructions);
    }
}  // namespace Horo::Navigation::TestSupport
