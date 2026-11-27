#pragma once

/** @file EnvironmentQueryGameplay.h @brief Bounded gameplay-owned point tests for cooked EQS. */

#include "Horo/AI/EnvironmentQueryExecution.h"

#include <atomic>

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

    /** @brief Exact owner and activation gates for a native image. */
    struct NativeGameplayQueryAdmission final {
        std::shared_ptr<const std::atomic_bool> current;
        std::shared_ptr<const std::atomic_bool> active;
    };

    /** @brief Returns the stable item identity accepted by native point tests. */
    [[nodiscard]] QueryItemTypeId BuiltinQueryPointItemType() noexcept;

    /** @brief Identifies gameplay-owned native test descriptors for the built-in point item. */
    [[nodiscard]] bool IsGameplayPointTest(const QueryTestDescriptor &descriptor) noexcept;

    /** @brief Appends or updates a native test, or removes an existing test when properties are absent. */
    [[nodiscard]] Result<EnvironmentQueryAsset> EditGameplayPointTest(const EnvironmentQueryAsset &source,
                                                                      const QuerySchemaRegistry &registry, QueryTestId test,
                                                                      QueryStageId stage,
                                                                      std::optional<std::span<const QueryPropertyValue>> properties);

    /** @brief Native game modules can export this optional versioned table alongside their descriptor bundle. */
    struct NativeGameplayQueryTestRegistration final {
        QueryTestDescriptor descriptor;
        GameplayQueryPointBinding binding;
    };

    inline constexpr std::string_view GetNativeGameplayQueryTestsSymbol = "GetHoroGameplayQueryTestsV1";

    /** @brief Returns a static table valid for the lifetime of the exporting native image. */
    using GetNativeGameplayQueryTests = const NativeGameplayQueryTestRegistration *(*)(std::size_t &count) noexcept;

    /** @brief Optional native declaration of tests that read only captured inputs and process input independent constants.
     * Qualified callbacks must be stateless, bounded and deterministic across supported builds. This augments the V1 test export.
     */
    inline constexpr std::string_view GetDeterministicGameplayQueryTestsSymbol = "GetHoroDeterministicGameplayQueryTestsV1";
    using GetDeterministicGameplayQueryTests = const QueryTestId *(*)(std::size_t &count) noexcept;

    /** @brief Freezes exact descriptor bindings and retains the owning code and data generation. */
    class GameplayQueryTestRegistry final {
    public:
        [[nodiscard]] static Result<GameplayQueryTestRegistry> Capture(
            const QuerySchemaRegistry &schema, std::span<const GameplayQueryPointBinding> bindings, std::shared_ptr<const void> generation,
            CancellationToken retirement = {}, NativeGameplayQueryAdmission native = {}, std::span<const QueryTestId> deterministic = {});
        [[nodiscard]] bool IsCurrent() const noexcept;
        [[nodiscard]] bool CanEvaluate() const noexcept;
        [[nodiscard]] bool Contains(QueryTestId test) const noexcept;
        [[nodiscard]] bool UsesTests(const EnvironmentQueryPlan &plan) const noexcept;
        [[nodiscard]] bool SupportsDeterministic(const EnvironmentQueryPlan &plan) const noexcept;

    private:
        friend Result<PreparedPointQueryTest> PrepareGameplayPointTest(const EnvironmentQueryPlan &, QueryStageId,
                                                                       const QuerySchemaRegistry &, const Runtime::RuntimeSceneView &,
                                                                       const QueryExecutionInputs &, const QueryExecutionSettings &,
                                                                       const std::shared_ptr<const GameplayQueryTestRegistry> &,
                                                                       const LivePointQueryGenerator *);

        [[nodiscard]] const GameplayQueryPointBinding *Find(QueryTestId test) const noexcept;
        std::vector<GameplayQueryPointBinding> bindings_;
        std::vector<QueryTestId> deterministic_;
        std::shared_ptr<const void> generation_;
        CancellationToken retirement_;
        NativeGameplayQueryAdmission native_;
    };

    /** @brief Composes one registered gameplay stage with a bounded built-in point generator. */
    [[nodiscard]] Result<PreparedPointQueryTest> PrepareGameplayPointTest(
        const EnvironmentQueryPlan &plan, QueryStageId stage, const QuerySchemaRegistry &schema, const Runtime::RuntimeSceneView &scene,
        const QueryExecutionInputs &inputs, const QueryExecutionSettings &settings,
        const std::shared_ptr<const GameplayQueryTestRegistry> &gameplay, const LivePointQueryGenerator *live);
}  // namespace Horo::AI
