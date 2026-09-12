#pragma once

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderMemoryTypes.h"
#include "Horo/Runtime/Render/RenderResource.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Render::Detail {
    enum class RenderResourceClass : std::uint8_t {
        Buffer,
        Texture,
        TextureView,
        Sampler,
        ShaderModule,
        Pipeline,
        RenderTarget,
        Mesh,
    };

    struct RenderResourceIdentity {
        RenderResourceOwnerId owner;
        std::uint32_t slot{0};
        std::uint32_t generation{0};

        [[nodiscard]] constexpr bool operator==(const RenderResourceIdentity &) const noexcept = default;
    };

    struct ResourceReservation {
        RenderResourceIdentity identity;
        ResourceOperationId operation;
    };

    struct RenderResourceRegistryLimits {
        std::uint32_t maximumSlots{65'535};
        std::uint32_t maximumPendingRequests{1'024};
        std::uint32_t retirementDrainBudget{64};
        std::uint32_t maximumOperationResults{4'096};

        [[nodiscard]] bool IsValid() const noexcept;
    };

    using BackendResourceRelease = std::function<void(RenderResourceClass resourceClass, std::uint64_t backendInstance,
                                                      std::optional<RenderMemoryAllocationId> memoryAllocation)>;

    /** @brief Frontend-private owner of resident identity, state, pins, and retirement. */
    class RenderResourceRegistry final {
    public:
        RenderResourceRegistry(RenderResourceOwnerId owner, RenderResourceRegistryLimits limits,
                               BackendResourceRelease releaseBackendResource = {});
        ~RenderResourceRegistry() = default;

        RenderResourceRegistry(const RenderResourceRegistry &) = delete;
        RenderResourceRegistry &operator=(const RenderResourceRegistry &) = delete;
        RenderResourceRegistry(RenderResourceRegistry &&) = delete;
        RenderResourceRegistry &operator=(RenderResourceRegistry &&) = delete;

        [[nodiscard]] Result<ResourceReservation> Reserve(RenderResourceClass resourceClass,
                                                          std::span<const RenderResourceIdentity> dependencies = {});
        [[nodiscard]] Result<void> Publish(RenderResourceClass resourceClass, RenderResourceIdentity identity,
                                           std::uint64_t backendInstance,
                                           std::optional<RenderMemoryAllocationId> memoryAllocation = std::nullopt);
        [[nodiscard]] Result<void> Fail(RenderResourceClass resourceClass, RenderResourceIdentity identity, Error error);
        [[nodiscard]] Result<void> Release(RenderResourceClass resourceClass, RenderResourceIdentity identity);
        [[nodiscard]] Result<void> CancelPending(RenderResourceClass resourceClass, RenderResourceIdentity identity);
        [[nodiscard]] Result<RenderResourceState> State(RenderResourceClass resourceClass, RenderResourceIdentity identity) const;
        [[nodiscard]] Result<void> OperationResult(ResourceOperationId operation) const;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderResourceClass resourceClass, RenderResourceIdentity identity) const;

        [[nodiscard]] Result<void> AddSubmissionPin(RenderResourceClass resourceClass, RenderResourceIdentity identity);
        [[nodiscard]] Result<void> ReleaseSubmissionPin(RenderResourceClass resourceClass, RenderResourceIdentity identity);
        [[nodiscard]] std::size_t DrainRetirements();
        void Shutdown() noexcept;

        [[nodiscard]] RenderResourceOwnerId Owner() const noexcept;

    private:
        struct Entry {
            RenderResourceClass resourceClass{RenderResourceClass::Buffer};
            RenderResourceState state{RenderResourceState::Retired};
            std::uint32_t generation{1};
            std::uint32_t dependentPins{0};
            std::uint32_t submissionPins{0};
            std::uint64_t backendInstance{0};
            std::optional<RenderMemoryAllocationId> memoryAllocation;
            ResourceOperationId operation;
            std::vector<RenderResourceIdentity> dependencies;
            bool generationExhausted{false};
            bool retirementQueued{false};
        };

        struct OperationRecord {
            ResourceOperationId id;
            bool complete{false};
            std::optional<Error> error;
        };

        [[nodiscard]] Result<std::size_t> Validate(RenderResourceClass resourceClass, RenderResourceIdentity identity) const;
        [[nodiscard]] Result<void> ValidateReservationAdmission() const;
        [[nodiscard]] Result<void> ValidateDependencies(std::span<const RenderResourceIdentity> dependencies) const;
        [[nodiscard]] Result<void> EnsureOperationResultCapacity();
        [[nodiscard]] Result<std::size_t> AcquireSlot();
        [[nodiscard]] const Entry *FindExact(RenderResourceIdentity identity) const noexcept;
        void CompleteOperation(ResourceOperationId operation, std::optional<Error> error);
        void QueueRetirementIfEligible(std::size_t slot);
        void Retire(std::size_t slot) noexcept;

        RenderResourceOwnerId owner_;
        RenderResourceRegistryLimits limits_;
        std::vector<Entry> entries_{{}};
        std::vector<std::uint32_t> freeSlots_;
        std::vector<std::uint32_t> retirementQueue_;
        std::size_t retirementQueueHead_{0};
        std::size_t retirementQueueCount_{0};
        std::deque<OperationRecord> operations_;
        std::uint64_t nextOperation_{1};
        std::uint32_t pendingRequests_{0};
        bool acceptingRequests_{true};
        BackendResourceRelease releaseBackendResource_;
    };

    [[nodiscard]] Result<RenderResourceOwnerId> AcquireRenderResourceOwnerId();
}  // namespace Horo::Render::Detail
