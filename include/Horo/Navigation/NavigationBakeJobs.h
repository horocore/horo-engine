#pragma once

/**
 * @file NavigationBakeJobs.h
 * @brief Cancellable structured navigation-bake jobs with bounded progress and resources.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Navigation {
    namespace NavigationBakeJobDetail {
        struct SharedState;
    }

    /** @brief Ordered owner stages in one navigation bake operation. */
    enum class NavigationBakeJobStage : std::uint8_t {
        PartitionGather,
        TileBuild,
        Validation,
        Publication,
        Count,
    };

    /** @brief Resource that prevented an admitted bake from executing within its profile. */
    enum class NavigationBakeBudgetResource : std::uint8_t {
        ConcurrentJobs,
        ResidentMemory,
        TemporaryStorage,
        WorkItems,
        WorkUnits,
        Count,
    };

    /** @brief Observable lifecycle owned by one accepted bake operation. */
    enum class NavigationBakeJobState : std::uint8_t {
        Queued,
        Running,
        Succeeded,
        Failed,
        Cancelled,
    };

    /** @brief Hard resource and drain bounds copied from one grounded bake profile. */
    struct NavigationBakeJobBudget final {
        std::size_t maximumConcurrentJobs{1};
        std::uint64_t maximumResidentBytes{1};
        std::uint64_t maximumTemporaryBytes{1};
        std::size_t maximumWorkItems{1};
        std::uint64_t maximumWorkUnits{1};
        Duration childDrainTimeout{Duration::FromMilliseconds(30'000)};
    };

    /** @brief One owned, bounded unit executed in its typed bake stage. */
    struct NavigationBakeWorkItem final {
        NavigationBakeJobStage stage{NavigationBakeJobStage::PartitionGather};
        std::uint64_t workUnits{1};
        std::uint64_t residentBytes{};
        std::uint64_t temporaryBytes{};
        JobFunction execute;
    };

    /** @brief Inputs copied before an asynchronous bake operation is made visible. */
    struct NavigationBakeJobDescriptor final {
        std::string title;
        NavigationBakeJobBudget budget;
        std::vector<NavigationBakeWorkItem> work;
        CancellationToken parentCancellation;
    };

    /** @brief Immutable polling projection of one accepted navigation bake. */
    struct NavigationBakeJobSnapshot final {
        OperationId operation{};
        NavigationBakeJobState state{NavigationBakeJobState::Queued};
        NavigationBakeJobStage stage{NavigationBakeJobStage::PartitionGather};
        std::uint64_t completedWorkUnits{};
        std::uint64_t totalWorkUnits{1};
        std::size_t acceptedChildJobs{};
        std::size_t terminalChildJobs{};
        std::optional<NavigationBakeBudgetResource> limitingResource;
        std::optional<Error> terminalError;
        std::uint64_t revision{1};

        /** @brief Reports whether no further snapshot mutation is possible. @return True for a terminal state. */
        [[nodiscard]] bool IsTerminal() const noexcept;
        /** @brief Returns operation-wide monotonic normalized progress. @return Value in [0, 1]. */
        [[nodiscard]] double Progress() const noexcept;
    };

    /** @brief Copyable non-blocking observation and cancellation capability. */
    class NavigationBakeJobHandle final {
    public:
        NavigationBakeJobHandle() = default;

        /** @brief Reports whether this handle references an accepted operation. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the application operation identity, or zero for an invalid handle. */
        [[nodiscard]] OperationId Id() const noexcept;
        /** @brief Copies the current operation state without waiting. */
        [[nodiscard]] std::optional<NavigationBakeJobSnapshot> Snapshot() const;
        /** @brief Requests cooperative cancellation without waiting for child jobs to drain. */
        [[nodiscard]] bool RequestCancellation() const noexcept;

    private:
        explicit NavigationBakeJobHandle(std::shared_ptr<NavigationBakeJobDetail::SharedState> state) noexcept;
        friend Result<NavigationBakeJobHandle> StartNavigationBakeJob(OperationStore &, JobSystem &, NavigationBakeJobDescriptor);
        std::shared_ptr<NavigationBakeJobDetail::SharedState> state_;
    };

    /**
     * @brief Admits and asynchronously executes one staged navigation bake through the process JobSystem.
     * @param operations Application-owned authoritative operation store.
     * @param jobs Process-owned scheduler; it must outlive the returned operation.
     * @param descriptor Owned stage work, cancellation ancestry and grounded resource budget.
     * @return Non-blocking operation handle, or a typed descriptor/store/scheduler admission error.
     * @pre `operations` and `jobs` outlive the returned operation and all accepted child work.
     */
    [[nodiscard]] Result<NavigationBakeJobHandle> StartNavigationBakeJob(OperationStore &operations, JobSystem &jobs,
                                                                         NavigationBakeJobDescriptor descriptor);
}  // namespace Horo::Navigation
