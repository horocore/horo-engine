#include "Horo/AI/EnvironmentQueryPerception.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/PerceptionSenseProvider.h"

#include <algorithm>
#include <array>
#include <bit>
#include <tuple>
#include <utility>

namespace Horo::AI {
    namespace {
        constexpr auto PerceptionProvider = 0x484f524f00030000ULL;
        constexpr QueryVersionRange Version{1, 1};
        constexpr auto Actor = PerceptionProvider + 1;
        constexpr auto Generator = PerceptionProvider + 2;
        constexpr auto ConfidenceTest = PerceptionProvider + 3;
        constexpr auto CurrentlySensed = PerceptionProvider + 4;

        using Generation = Result<QueryGenerationResult>;
        using Admission = Result<std::unique_ptr<QueryExecution>>;

        QueryDescriptorOrigin NativeOrigin() {
            return {.provider = QueryProviderId::Create(PerceptionProvider).Value(), .version = Version.minimum};
        }

        template <class T> Result<T> KernelFailure(const ErrorCodeDescriptor &error) noexcept {
            return Result<T>::Failure({.code = error.code, .domain = error.domain, .severity = error.defaultSeverity});
        }

        [[nodiscard]] bool ValidTest(const QueryPerceptionTest &test) noexcept {
            return test.stage.IsValid() && test.agent.IsValid() && test.revision != 0 &&
                   test.match <= QueryPerceptionMatch::CurrentlySensed && (!test.sense || test.sense->IsValid());
        }

        [[nodiscard]] bool Matches(const PerceivedStimulus &entry, const PerceptionSourceRef &candidate,
                                   const QueryPerceptionTest &test) noexcept {
            return entry.key.source == candidate && (!test.sense || entry.key.sense == *test.sense) &&
                   (test.match == QueryPerceptionMatch::Remembered || entry.isCurrentlySensed);
        }

        /** @brief Reduces all matching observations without consulting provider state. */
        QueryTestSample TestMemory(const PerceptionMemorySnapshot &memory, const QueryPerceptionTest &test,
                                   const Runtime::EntityRef candidate) {
            const auto projected = ProjectPerceptionSource(candidate);
            QueryTestSample sample{.stage = test.stage, .filter = false, .metric = 0.0};
            for (const auto &entry : std::span{memory.entries}.first(memory.count)) {
                if (!Matches(entry, projected, test))
                    continue;
                sample.filter = true;
                sample.metric = std::max(*sample.metric, entry.confidence);
            }
            return sample;
        }

        /** @brief Assigns stable actor order independently of the order of perception senses. */
        Generation ProjectActors(const PerceptionMemorySnapshot &memory, const Runtime::SceneRuntimeId scene,
                                 const QueryGeneratorOutput &output) {
            std::array<PerceptionSourceRef, MaximumPerceptionMemoryEntries> records{};
            auto candidates = std::span{records}.first(memory.count);
            std::ranges::transform(std::span{memory.entries}.first(memory.count), candidates.begin(), [](const auto &record) {
                return record.key.source;
            });
            std::ranges::sort(candidates, {}, [](const auto &source) {
                return std::tuple{source.slot, source.generation};
            });
            const auto duplicates = std::ranges::unique(candidates);
            const auto count = static_cast<std::size_t>(duplicates.begin() - candidates.begin());
            if (count > output.maximumItems)
                return KernelFailure<QueryGenerationResult>(AIErrors::EnvironmentQueryLimitExceeded);
            std::ranges::transform(candidates.first(count), output.items.begin(), [scene](const auto &source) {
                return Runtime::EntityRef{scene, {source.slot, source.generation}};
            });
            return Generation::Success({.provenance = output.provenance, .itemCount = count});
        }

        /** @brief Validates a detached perception read without retaining the provider owner. */
        bool IsAuthorized(const PerceptionQueryInput &input) noexcept {
            const auto provider = input.provider.lock();
            return input.authorization && input.authorization->IsCurrent() && provider && provider->CanAdmit();
        }

        constexpr QueryOwnerWorkLimits PerceptionWork{
            MaximumPerceptionMemoryEntries * (3U + std::bit_width(MaximumPerceptionMemoryEntries)),
            MaximumPerceptionMemoryEntries,
        };

        /** @brief Source over one fixed, filtered memory snapshot and its revocable leases. */
        class PerceptionOwnerSource final : public IOwnerQueryExecutionSource {
        public:
            PerceptionOwnerSource(PerceptionQueryInput input, QueryPerceptionTest test)
                : IOwnerQueryExecutionSource(PerceptionWork), input_(std::move(input)), test_(test) {}

            bool IsCurrent(const QueryExecutionInputs &inputs) const noexcept override {
                return inputs.contexts->MatchesScene(input_.scene) && IsAuthorized(input_);
            }

        private:
            Generation GenerateAtOwner(const QueryExecutionInputs &, const QueryGeneratorOutput &output,
                                       AiWorkBudget &budget) noexcept override {
                if (!budget.Consume(PerceptionWork.generationUnits))
                    return KernelFailure<QueryGenerationResult>(AIErrors::SchedulerBudgetExhausted);
                return ProjectActors(input_.memory, input_.scene.RuntimeId(), output);
            }

            Result<QueryTestSample> TestAtOwner(const QueryExecutionInputs &, QueryStageId stage, const QueryGeneratedItem &item,
                                                AiWorkBudget &budget) noexcept override {
                using Sample = Result<QueryTestSample>;
                if (!budget.Consume(PerceptionWork.testUnits))
                    return KernelFailure<QueryTestSample>(AIErrors::SchedulerBudgetExhausted);
                const auto *actor = std::get_if<Runtime::EntityRef>(&item);
                if (!actor || stage != test_.stage)
                    return KernelFailure<QueryTestSample>(AIErrors::EnvironmentQuerySchemaInvalid);
                return Sample::Success(TestMemory(input_.memory, test_, *actor));
            }

            PerceptionQueryInput input_;
            QueryPerceptionTest test_;
        };

        /** @brief Resolves the single native filter/score stage from the immutable plan. */
        Result<QueryPerceptionMatch> CaptureMatch(const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry) {
            using Match = Result<QueryPerceptionMatch>;
            const auto stages = plan.Stages();
            if (stages.size() != 2 || !UsesBuiltinPerceptionQuery(plan) ||
                plan.ResultSchema().itemType != QueryItemTypeId::Create(Actor).Value() ||
                stages[1].test != QueryTestId::Create(ConfidenceTest).Value())
                return Match::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            const auto *generator = registry.Find(stages.front().generator);
            const auto *test = registry.Find(stages[1].test);
            if (!generator || !test || generator->origin != NativeOrigin() || test->origin != NativeOrigin())
                return Match::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            const auto required =
                std::ranges::find(stages[1].properties, QueryPropertyId::Create(CurrentlySensed).Value(), &QueryPropertyValue::id);
            const auto *value = required == stages[1].properties.end() ? nullptr : std::get_if<bool>(&required->value);
            if (!value)
                return Match::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            return Match::Success(*value ? QueryPerceptionMatch::CurrentlySensed : QueryPerceptionMatch::Remembered);
        }
    }  // namespace

    /** @copydoc EvaluateQueryPerception */
    Result<QueryTestSample> EvaluateQueryPerception(const QueryPerceivedEntitySource &source, const QueryPerceptionTest &test,
                                                    const Runtime::EntityRef candidate) {
        using TestResult = Result<QueryTestSample>;
        if (!ValidTest(test))
            return TestResult::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        auto current = source.publication.Current(source.scene, source.filtering, source.blackboard);
        if (current.HasError())
            return TestResult::Failure(std::move(current).ErrorValue());
        const auto &publication = current.Value();
        if (publication.agent != test.agent || publication.revision != test.revision || source.scene.Get(candidate).HasError())
            return TestResult::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        return TestResult::Success(TestMemory(publication.memory, test, candidate));
    }

    /** @copydoc GenerateQueryPerceivedEntities */
    Result<QueryGenerationResult> GenerateQueryPerceivedEntities(const QueryPerceivedEntitySource &source,
                                                                 const QueryGeneratorOutput &output) {
        if (!output.IsValid())
            return Generation::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        const auto current = source.publication.Current(source.scene, source.filtering, source.blackboard);
        if (current.HasError())
            return Generation::Failure(current.ErrorValue());
        if (current.Value().revision != output.provenance.revision)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        return ProjectActors(current.Value().memory, source.scene.RuntimeId(), output);
    }

    /** @copydoc DescribeBuiltinPerceptionQuery */
    BuiltinPerceptionQueryDescriptors DescribeBuiltinPerceptionQuery() {
        const auto origin = NativeOrigin();
        const auto item = QueryItemTypeId::Create(Actor).Value();
        return {.item = {.id = item, .origin = origin, .kind = QueryItemKind::Actor},
                .generator = {.id = QueryGeneratorId::Create(Generator).Value(),
                              .origin = origin,
                              .outputItemType = item,
                              .outputItemVersion = Version},
                .test = {.id = QueryTestId::Create(ConfidenceTest).Value(),
                         .origin = origin,
                         .inputItemType = item,
                         .inputItemVersion = Version,
                         .properties = {{QueryPropertyId::Create(CurrentlySensed).Value(), QueryPropertyKind::Boolean, true}}}};
    }

    /** @copydoc CreateBuiltinPerceptionQuery */
    Result<EnvironmentQueryAsset> CreateBuiltinPerceptionQuery(QueryId query, QueryStageId generator, QueryStageId test,
                                                               QueryResultSchemaId result, std::string displayName,
                                                               QueryPerceptionMatch match, std::uint32_t maximumResults) {
        if (match > QueryPerceptionMatch::CurrentlySensed)
            return Result<EnvironmentQueryAsset>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        return EnvironmentQueryAsset::Capture({.id = query,
                                               .displayName = std::move(displayName),
                                               .result = {.id = result,
                                                          .version = Version.minimum,
                                                          .itemType = QueryItemTypeId::Create(Actor).Value(),
                                                          .itemVersion = Version,
                                                          .maximumResults = maximumResults},
                                               .stages = {{.id = generator,
                                                           .kind = QueryStageKind::Generator,
                                                           .generator = QueryGeneratorId::Create(Generator).Value(),
                                                           .descriptorVersion = Version},
                                                          {.id = test,
                                                           .kind = QueryStageKind::Test,
                                                           .executionOrder = 1,
                                                           .test = QueryTestId::Create(ConfidenceTest).Value(),
                                                           .descriptorVersion = Version,
                                                           .properties = {{QueryPropertyId::Create(CurrentlySensed).Value(),
                                                                           match == QueryPerceptionMatch::CurrentlySensed}}}}});
    }

    /** @copydoc UsesBuiltinPerceptionQuery */
    bool UsesBuiltinPerceptionQuery(const EnvironmentQueryPlan &plan) noexcept {
        const auto stages = plan.Stages();
        return !stages.empty() && stages.front().generator == QueryGeneratorId::Create(Generator).Value();
    }

    /** @copydoc AdmitBuiltinPerceptionQuery */
    Admission AdmitBuiltinPerceptionQuery(const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry,
                                          QueryExecutionInputs inputs, PerceptionQueryInput perception, QueryExecutionSettings settings) {
        if (!inputs.contexts || inputs.contexts->Query() != plan.Id() || perception.agent != inputs.agent ||
            perception.spatialRevision == 0 || perception.memory.count > MaximumPerceptionMemoryEntries)
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        if (!inputs.contexts->MatchesScene(perception.scene) || !IsAuthorized(perception))
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto match = CaptureMatch(plan, registry);
        if (match.HasError())
            return Admission::Failure(match.ErrorValue());
        const auto stages = plan.Stages();
        const QueryScoreRule rule{.stage = stages[1].id, .requiredFilterValue = true, .score = QueryScoreRange{}};
        auto scoring = QueryScoringPlan::Capture(plan, registry, std::span{&rule, 1}, QueryScoreTieBreak::StableIndex);
        if (scoring.HasError())
            return Admission::Failure(std::move(scoring).ErrorValue());
        const QueryGeneratorProvenance provenance{stages.front().id, stages.front().generator,
                                                  registry.Find(stages.front().generator)->origin.provider, perception.spatialRevision};
        const QueryPerceptionTest test{stages[1].id, inputs.agent, perception.spatialRevision, match.Value()};
        settings.maximumCandidates = std::min(settings.maximumCandidates, MaximumPerceptionMemoryEntries);
        settings.generatorWorkUnits = PerceptionWork.generationUnits;
        inputs.outputAuthorization = perception.authorization;
        auto source = std::make_unique<PerceptionOwnerSource>(std::move(perception), test);
        return QueryExecution::AdmitDeterministic(std::move(inputs), provenance, std::move(scoring).Value(), settings, std::move(source));
    }
}  // namespace Horo::AI
