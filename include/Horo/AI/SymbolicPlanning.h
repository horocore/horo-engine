#pragma once

/** @file SymbolicPlanning.h @brief Bounded symbolic planning over inert, registered task declarations. */

#include "Horo/AI/AICapabilityDescriptors.h"
#include "Horo/AI/AITaskJobService.h"

#include <atomic>

namespace Horo::AI {
    using SymbolicFactId = AiStableIdentity<struct SymbolicFactIdentityTag>;
    using SymbolicActionId = AiStableIdentity<struct SymbolicActionIdentityTag>;

    struct SymbolicLiteral final {
        SymbolicFactId fact;
        bool value{};
    };

    /** @brief Positive-cost transition. Spans are borrowed only by domain capture. */
    struct SymbolicActionSource final {
        SymbolicActionId action;
        TaskId task;
        std::uint32_t cost{};
        std::span<const SymbolicLiteral> preconditions;
        std::span<const SymbolicLiteral> effects;
    };

    struct SymbolicPlanStep final {
        SymbolicActionId action;
        TaskId task; /**< Inert registered task identity. Execution remains with the decision owner. */
    };

    enum class SymbolicPlanStatus : std::uint8_t { Solved, NoPlan, BudgetExhausted, Cancelled, InvalidInput };

    /** @brief Owns a complete ordered task plan. Unsolved outcomes carry no executable prefix. */
    class SymbolicPlan final {
    public:
        static constexpr std::size_t MaximumStates = 256;
        [[nodiscard]] SymbolicPlanStatus Status() const noexcept { return status_; }
        [[nodiscard]] std::uint64_t Cost() const noexcept { return cost_; }
        [[nodiscard]] std::span<const SymbolicPlanStep> Steps() const & noexcept {
            return std::span{steps_}.first(count_);
        }
        [[nodiscard]] std::span<const SymbolicPlanStep> Steps() const && = delete;

    private:
        friend class SymbolicDomain;
        SymbolicPlanStatus status_{SymbolicPlanStatus::InvalidInput};
        std::uint64_t cost_{};
        std::array<SymbolicPlanStep, MaximumStates - 1> steps_{};
        std::size_t count_{};
    };

    /** @brief Immutable Boolean fact and registered task schema for an opt-in planning provider. */
    class SymbolicDomain final {
    public:
        static constexpr std::size_t MaximumFacts = 64;
        static constexpr std::size_t MaximumActions = 64;
        static constexpr std::size_t MaximumWork = 1U << 20U;

        /** @brief Copies the schema after rejecting invalid identities, inconsistent literals and unregistered tasks. */
        [[nodiscard]] static Result<SymbolicDomain> Capture(
            std::span<const SymbolicFactId> facts, std::span<const SymbolicActionSource> actions,
            const AiCapabilityDescriptorSet &tasks, std::optional<BlackboardSchemaId> blackboard = {});

        /** @brief Searches detached values with a finite work budget. Unspecified initial facts are false. */
        [[nodiscard]] SymbolicPlan Search(std::span<const SymbolicLiteral> initial, std::span<const SymbolicLiteral> goal,
                                          std::size_t work, const CancellationToken &cancellation = {}) const noexcept;

    private:
        friend class SymbolicPlanningJob;
        SymbolicDomain() = default;
        struct Mask final {
            std::uint64_t known{};
            std::uint64_t values{};
            [[nodiscard]] bool Matches(std::uint64_t state) const noexcept { return (state & known) == values; }
        };
        struct Action final {
            SymbolicPlanStep identity;
            std::uint32_t cost{};
            Mask preconditions;
            Mask effects;
        };
        struct Frontier;

        [[nodiscard]] std::optional<Mask> Encode(std::span<const SymbolicLiteral> literals) const noexcept;
        [[nodiscard]] SymbolicPlan SearchImpl(std::span<const SymbolicLiteral> initial, std::span<const SymbolicLiteral> goal,
                                              std::size_t work, const CancellationToken &cancellation,
                                              const AiTaskOperationContext *operation) const noexcept;
        std::array<SymbolicFactId, MaximumFacts> facts_{};
        std::array<Action, MaximumActions> actions_{};
        std::size_t factCount_{};
        std::size_t actionCount_{};
    };

    /** @brief One-shot detached search for the existing AI task job service. */
    class SymbolicPlanningJob final : public IAiTaskJob {
    public:
        [[nodiscard]] static Result<std::shared_ptr<SymbolicPlanningJob>> Create(
            std::shared_ptr<const SymbolicDomain> domain, std::span<const SymbolicLiteral> initial,
            std::span<const SymbolicLiteral> goal, std::size_t work);

        [[nodiscard]] Result<void> Run(const AiTaskOperationContext &operation, const CancellationToken &cancellation) const override;
        /** @brief Reads an immutable result after the task owner receives completion. */
        [[nodiscard]] const SymbolicPlan *Read() const noexcept;

    private:
        SymbolicPlanningJob() = default;
        enum class State : std::uint8_t { Ready, Running, Complete };
        std::shared_ptr<const SymbolicDomain> domain_;
        std::array<SymbolicLiteral, SymbolicDomain::MaximumFacts> initial_{};
        std::array<SymbolicLiteral, SymbolicDomain::MaximumFacts> goal_{};
        std::size_t initialCount_{};
        std::size_t goalCount_{};
        std::size_t work_{};
        mutable std::atomic<State> state_{State::Ready};
        mutable SymbolicPlan result_;
    };
}  // namespace Horo::AI
