#pragma once

/** @file EnvironmentQueryGameplay.h @brief Bounded gameplay-owned point tests for cooked EQS. */

#include "Horo/AI/EnvironmentQueryExecution.h"

namespace Horo::AI {
    /** @brief Bounded, immutable observations offered for one authored candidate test. */
    struct GameplayQueryPointInput final {
        const QueryContextSnapshot &contexts;
        QueryStageId stage;
        Math::WorldCoordinate64 point;
        std::span<const QueryPropertyValue> properties;
    };

    /** @brief Reads captured query inputs and provider data on the Scene owner lane. */
    using GameplayQueryPointTest = QueryTestSample (*)(const void *, const GameplayQueryPointInput &, AiWorkBudget &) noexcept;

    /** @brief Associates one installed gameplay test descriptor with its pure kernel and score policy. */
    struct GameplayQueryPointBinding final {
        QueryTestId test;
        QueryDescriptorOrigin origin;
        std::optional<bool> requiredFilter;
        std::optional<QueryScoreRange> score;
        std::size_t workUnits{};
        const void *state{};
        GameplayQueryPointTest evaluate{};
    };

    /** @brief Freezes exact descriptor bindings and retains the owning code and data generation. */
    class GameplayQueryTestRegistry final {
    public:
        [[nodiscard]] static Result<GameplayQueryTestRegistry> Capture(const QuerySchemaRegistry &schema,
                                                                       std::span<const GameplayQueryPointBinding> bindings,
                                                                       std::shared_ptr<const void> generation,
                                                                       CancellationToken retirement = {});
        [[nodiscard]] bool IsCurrent() const noexcept;
        [[nodiscard]] const GameplayQueryPointBinding *Find(QueryTestId test) const noexcept;
        [[nodiscard]] bool UsesTests(const EnvironmentQueryPlan &plan) const noexcept;

    private:
        std::vector<GameplayQueryPointBinding> bindings_;
        std::shared_ptr<const void> generation_;
        CancellationToken retirement_;
    };

    /** @brief Composes one registered gameplay stage with a bounded built-in point generator. */
    [[nodiscard]] Result<PreparedPointQueryTest> PrepareGameplayPointTest(
        const EnvironmentQueryPlan &plan, QueryStageId stage, const QuerySchemaRegistry &schema, const Runtime::RuntimeSceneView &scene,
        const QueryExecutionInputs &inputs, const QueryExecutionSettings &settings,
        const std::shared_ptr<const GameplayQueryTestRegistry> &gameplay, const LivePointQueryGenerator *live);
}  // namespace Horo::AI
