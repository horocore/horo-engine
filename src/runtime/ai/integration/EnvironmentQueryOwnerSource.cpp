#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryExecution.h"

#include <utility>

namespace Horo::AI {
    /** @copydoc IOwnerQueryExecutionSource::IOwnerQueryExecutionSource */
    IOwnerQueryExecutionSource::IOwnerQueryExecutionSource(const QueryOwnerWorkLimits limits) noexcept : limits_(limits) {}

    /** @copydoc IOwnerQueryExecutionSource::WorkLimits */
    QueryOwnerWorkLimits IOwnerQueryExecutionSource::WorkLimits() const noexcept {
        return limits_;
    }

    /** @copydoc IOwnerQueryExecutionSource::Generate */
    Result<QueryGenerationResult> IOwnerQueryExecutionSource::Generate(const QueryExecutionInputs &inputs,
                                                                       const QueryGeneratorOutput &output) {
        AiWorkBudget budget{limits_.generationUnits};
        auto generated = GenerateAtOwner(inputs, output, budget);
        if (budget.Exhausted())
            return Result<QueryGenerationResult>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
        return generated;
    }

    /** @copydoc IOwnerQueryExecutionSource::StartTest */
    Result<bool> IOwnerQueryExecutionSource::StartTest(const QueryExecutionInputs &inputs, const QueryStageId stage,
                                                       const QueryGeneratedItem &item) {
        if (completion_)
            return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        AiWorkBudget budget{limits_.testUnits};
        auto sample = TestAtOwner(inputs, stage, item, budget);
        if (budget.Exhausted())
            sample = Result<QueryTestSample>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
        completion_.emplace(std::move(sample));
        return Result<bool>::Success(true);
    }

    /** @copydoc IOwnerQueryExecutionSource::PollTest */
    Result<std::optional<QueryTestSample>> IOwnerQueryExecutionSource::PollTest() {
        using Sample = Result<std::optional<QueryTestSample>>;
        if (!completion_)
            return Sample::Success(std::nullopt);
        auto completed = std::move(*completion_);
        completion_.reset();
        if (completed.HasError())
            return Sample::Failure(std::move(completed).ErrorValue());
        return Sample::Success(std::move(completed).Value());
    }

    /** @copydoc IOwnerQueryExecutionSource::CancelTest */
    void IOwnerQueryExecutionSource::CancelTest() noexcept {
        completion_.reset();
    }
}  // namespace Horo::AI
