#pragma once

/** @file SymbolicPlanning.h @brief Bounded symbolic planning over inert, registered task declarations. */

#include "Horo/AI/AICapabilityDescriptors.h"
#include "Horo/AI/AITaskJobService.h"
#include "Horo/AI/DecisionIdentity.h"

#include <atomic>
#include <variant>

namespace Horo::AI {
    class BehaviorTreeExecutionPlan;
    class DecisionAssetPlan;

    using SymbolicFactId = AiStableIdentity<struct SymbolicFactIdentityTag>;
    using SymbolicActionId = AiStableIdentity<struct SymbolicActionIdentityTag>;
    using SymbolicCompoundId = AiStableIdentity<struct SymbolicCompoundIdentityTag>;
    using SymbolicMethodId = AiStableIdentity<struct SymbolicMethodIdentityTag>;

    struct SymbolicLiteral final {
        SymbolicFactId fact;
        bool value{};
    };

    /** @brief Positive-cost action. Spans are borrowed only by domain capture.
     * @details Empty effects leave the symbolic state unchanged. HTN can order
     * the action explicitly, and GOAP omits transitions that leave the state unchanged.
     */
    struct SymbolicActionSource final {
        SymbolicActionId action;
        TaskId task;
        std::uint32_t cost{};
        std::span<const SymbolicLiteral> preconditions;
        std::span<const SymbolicLiteral> effects;
    };

    /** @brief Inert action and exact task declaration admitted by the planning domain. */
    struct SymbolicPlanStep final {
        SymbolicActionId action;
        TaskId task; /**< Inert registered task identity. Execution remains with the decision owner. */
        std::optional<BlackboardSchemaId> blackboard; /**< Schema requirement when the task was admitted. */
        AiTaskAuthoring authoring{AiTaskAuthoring::Native}; /**< Authored task kind when the task was admitted. */
    };

    enum class SymbolicPlanStatus : std::uint8_t {
        Solved,
        NoPlan,
        BudgetExhausted,
        Cancelled,
        InvalidInput
    };

    /** @brief Owns a complete ordered task plan. Unsolved outcomes carry no executable prefix. */
    class SymbolicPlan final {
    public:
        static constexpr std::size_t MaximumStates = 256;

        [[nodiscard]] SymbolicPlanStatus Status() const noexcept {
            return status_;
        }

        [[nodiscard]] std::uint64_t Cost() const noexcept {
            return cost_;
        }

        [[nodiscard]] std::span<const SymbolicPlanStep> Steps() const & noexcept {
            return std::span{steps_}.first(count_);
        }

        [[nodiscard]] std::span<const SymbolicPlanStep> Steps() const && = delete;

    private:
        friend class SymbolicDomain;
        friend class SymbolicHierarchy;
        friend class SymbolicPlanningJob;
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
        [[nodiscard]] static Result<SymbolicDomain> Capture(std::span<const SymbolicFactId> facts,
                                                            std::span<const SymbolicActionSource> actions,
                                                            const AiCapabilityDescriptorSet &tasks,
                                                            std::optional<BlackboardSchemaId> blackboard = {});

        /** @brief Searches detached values with a finite work budget. Unspecified initial facts are false. */
        [[nodiscard]] SymbolicPlan Search(std::span<const SymbolicLiteral> initial, std::span<const SymbolicLiteral> goal, std::size_t work,
                                          const CancellationToken &cancellation = {}) const noexcept;

    private:
        friend class SymbolicPlanningJob;
        friend class SymbolicHierarchy;
        SymbolicDomain() = default;

        struct Mask final {
            std::uint64_t known{};
            std::uint64_t values{};

            [[nodiscard]] bool Matches(std::uint64_t state) const noexcept {
                return (state & known) == values;
            }
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

    using SymbolicSubtask = std::variant<SymbolicActionId, SymbolicCompoundId>;

    /** @brief Guarded HTN method. Child tasks execute in authored order. */
    struct SymbolicMethodSource final {
        SymbolicMethodId method;
        SymbolicCompoundId compound;
        std::span<const SymbolicLiteral> preconditions;
        std::span<const SymbolicSubtask> children;
    };

    /** @brief Deterministic HTN decomposition using the same registered primitives as symbolic search. */
    class SymbolicHierarchy final {
    public:
        static constexpr std::size_t MaximumMethods = 64;
        static constexpr std::size_t MaximumChildren = 16;
        static constexpr std::size_t MaximumDepth = 64;

        /** @brief Copies method definitions and verifies every primitive and compound reference. */
        [[nodiscard]] static Result<SymbolicHierarchy> Capture(std::shared_ptr<const SymbolicDomain> primitives,
                                                               std::span<const SymbolicMethodSource> methods);

        /** @brief Tries applicable methods in stable method-ID order and returns the first complete plan. */
        [[nodiscard]] SymbolicPlan Search(std::span<const SymbolicLiteral> initial, SymbolicCompoundId root, std::size_t work,
                                          const CancellationToken &cancellation = {}) const noexcept;

    private:
        friend class SymbolicPlanningJob;
        struct Decomposition;

        struct Method final {
            SymbolicMethodId id;
            SymbolicCompoundId compound;
            SymbolicDomain::Mask preconditions;
            std::array<SymbolicSubtask, MaximumChildren> children{};
            std::size_t childCount{};
        };

        SymbolicHierarchy() = default;
        [[nodiscard]] bool HasCompound(SymbolicCompoundId compound) const noexcept;
        [[nodiscard]] const SymbolicDomain::Action *FindAction(SymbolicActionId action) const noexcept;
        [[nodiscard]] SymbolicPlan SearchImpl(std::span<const SymbolicLiteral> initial, SymbolicCompoundId root, std::size_t work,
                                              const CancellationToken &cancellation,
                                              const AiTaskOperationContext *operation) const noexcept;
        std::shared_ptr<const SymbolicDomain> primitives_;
        std::array<Method, MaximumMethods> methods_{};
        std::size_t methodCount_{};
    };

    /** @brief One host-admitted node occurrence for an exact primitive action and task. */
    struct SymbolicTaskOccurrence final {
        SymbolicActionId action;
        TaskId task;
        DecisionNodeId node;
    };

    /** @brief Exact ordered occurrence bindings borrowed only while compiling. */
    struct SymbolicExecutionBindings final {
        std::shared_ptr<const DecisionAssetPlan> asset;
        DecisionNodeId sequence;
        std::span<const SymbolicTaskOccurrence> occurrences;
    };

    /**
     * @brief Converts a solved plan into an ordinary admitted Behavior Tree task sequence without invoking providers.
     * @param plan Detached result the caller has accepted for the current world revision.
     * @param bindings Exact decision asset with an intrinsic Sequence root and provider-bound task occurrences.
     * @param tasks Current frozen task declarations for that owner.
     * @return Immutable execution plan, null for a solved empty plan, or a typed admission failure.
     */
    [[nodiscard]] Result<std::shared_ptr<const BehaviorTreeExecutionPlan>> CompileSymbolicTaskSequence(
        const SymbolicPlan &plan, SymbolicExecutionBindings bindings, const AiCapabilityDescriptorSet &tasks);

    /** @brief One-shot detached search for the existing AI task job service. */
    class SymbolicPlanningJob final : public IAiTaskJob {
    public:
        [[nodiscard]] static Result<std::shared_ptr<SymbolicPlanningJob>> Create(std::shared_ptr<const SymbolicDomain> domain,
                                                                                 std::span<const SymbolicLiteral> initial,
                                                                                 std::span<const SymbolicLiteral> goal, std::size_t work);
        [[nodiscard]] static Result<std::shared_ptr<SymbolicPlanningJob>> Create(std::shared_ptr<const SymbolicHierarchy> hierarchy,
                                                                                 std::span<const SymbolicLiteral> initial,
                                                                                 SymbolicCompoundId root, std::size_t work);

        [[nodiscard]] Result<void> Run(const AiTaskOperationContext &operation, const CancellationToken &cancellation) const override;
        /** @brief Reads the immutable result at the task owner's safe point.
         * @return Null before completion or if later cancellation withdrew the outcome. An explicitly cancelled search remains readable.
         * @details Reacquire at each use. A previously borrowed pointer does not grant authority to act on the outcome.
         */
        [[nodiscard]] const SymbolicPlan *Read() const noexcept;

    private:
        SymbolicPlanningJob() = default;
        [[nodiscard]] static Result<std::shared_ptr<SymbolicPlanningJob>> CaptureInputs(std::span<const SymbolicLiteral> initial,
                                                                                        std::size_t work);
        enum class State : std::uint8_t {
            Ready,
            Running,
            Complete
        };
        std::shared_ptr<const SymbolicDomain> domain_;
        std::shared_ptr<const SymbolicHierarchy> hierarchy_;
        SymbolicCompoundId root_;
        std::array<SymbolicLiteral, SymbolicDomain::MaximumFacts> initial_{};
        std::array<SymbolicLiteral, SymbolicDomain::MaximumFacts> goal_{};
        std::size_t initialCount_{};
        std::size_t goalCount_{};
        std::size_t work_{};
        // The worker writes the tokens and result once before publishing Complete to readers.
        mutable std::array<CancellationToken, 4> resultCancellation_;
        mutable std::atomic<State> state_{State::Ready};
        mutable SymbolicPlan result_;
    };
}  // namespace Horo::AI
