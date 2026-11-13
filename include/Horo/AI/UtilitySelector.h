#pragma once

/**
 * @file UtilitySelector.h
 * @brief Bounded utility scoring over immutable, schema-bound blackboard values.
 */

#include "Horo/AI/AITaskScheduler.h"
#include "Horo/AI/BlackboardInstance.h"
#include "Horo/AI/DecisionControl.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::AI {
    /** @brief Response applied to a normalized scalar. */
    enum class UtilityResponseCurve : std::uint8_t {
        Linear,
        Power,
        Logistic,
        Step,
        Count,
    };

    /** @brief One typed scalar key and its fixed-range utility policy. */
    struct UtilityConsideration final {
        BlackboardKeyId key;
        double minimum{};
        double maximum{1};
        double weight{1};
        UtilityResponseCurve curve{UtilityResponseCurve::Linear};
        double parameter{1}; /**< Exponent, logistic slope, or normalized step threshold. */
        bool invert{};
    };

    /** @brief One stable action and the bounded considerations that contribute to its priority. */
    struct UtilityOption final {
        DecisionNodeId id;
        TaskId action;
        double priority{1};
        double compensation{}; /**< Fraction of the multiplier compensation to apply. */
        std::vector<UtilityConsideration> considerations;
    };

    /** @brief Redacted normalized score evidence. An invalid key identifies an option total. */
    struct UtilityScoreRecord final {
        DecisionNodeId option;
        BlackboardKeyId key;
        std::optional<double> score; /**< Empty if the consideration or option was ineligible. */
    };

    /** @brief One selected stable action, with its finite normalized score. */
    struct UtilitySelection final {
        DecisionNodeId option;
        TaskId action;
        double score{};
    };

    /** @brief Selection and bounded evidence accounting for one evaluation. */
    struct UtilityEvaluation final {
        std::optional<UtilitySelection> selection;
        std::uint64_t blackboardRevision{};
        std::size_t evidenceWritten{};
        std::size_t evidenceDropped{};
        bool budgetExhausted{};
    };

    /** @brief Immutable utility plan with stable option-ID ties and all-or-nothing budget reservation. */
    class UtilitySelector final {
    public:
        static constexpr std::size_t MaximumOptions = 64;
        static constexpr std::size_t MaximumConsiderations = 16;

        /** @brief Captures and validates the complete authored selector before evaluation.
         * @param schema Exact admitted immutable blackboard schema. @param options Bounded authored options.
         * @return Owned selector or typed invalid/limit failure.
         * @throws std::bad_alloc If bounded option storage cannot be allocated.
         */
        [[nodiscard]] static Result<UtilitySelector> Capture(std::shared_ptr<const BlackboardSchema> schema,
                                                             std::span<const UtilityOption> options);

        /** @brief Evaluates the selector without dispatching the selected task.
         * @param snapshot Frozen blackboard publication. @param agent Exact owning agent.
         * @param agentBudget Current agent allowance. @param globalBudget Current global allowance.
         * @param evidence Optional bounded caller storage in stable option-ID order.
         * @return Selection, exhausted allowance or typed stale-input failure.
         */
        [[nodiscard]] Result<UtilityEvaluation> Evaluate(const BlackboardSnapshot &snapshot, AgentHandle agent, AiWorkBudget &agentBudget,
                                                         AiWorkBudget &globalBudget, std::span<UtilityScoreRecord> evidence = {}) const;

        /** @brief Returns validated options in the declared stable order. @return Immutable policies. */
        [[nodiscard]] std::span<const UtilityOption> Options() const noexcept;
        /** @brief Measures owned selector storage without counting its borrowed schema lease. @return Logical storage. */
        [[nodiscard]] DecisionKernelStorage Storage() const noexcept;

    private:
        UtilitySelector() = default;
        std::shared_ptr<const BlackboardSchema> schema_;
        std::vector<UtilityOption> options_;
        std::size_t workUnits_{};
    };
}  // namespace Horo::AI
