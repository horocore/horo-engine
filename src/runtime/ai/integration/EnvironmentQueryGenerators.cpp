#include "Horo/AI/EnvironmentQueryGenerators.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>
#include <utility>

namespace Horo::AI {
    namespace {
        using Generation = Result<QueryGenerationResult>;

        // Stable native contribution identities are shared by descriptor capture, initial authoring and dispatch.
        constexpr std::uint64_t LatticeProvider = 0x484f524f00020000ULL;
        constexpr std::uint64_t PointItem = LatticeProvider + 1;
        constexpr std::uint64_t LatticeGenerator = LatticeProvider + 2;
        constexpr std::uint64_t StepMillimeters = LatticeProvider + 3;
        constexpr std::uint64_t OuterSteps = LatticeProvider + 4;
        constexpr std::uint64_t InnerSteps = LatticeProvider + 5;
        constexpr QueryVersionRange LatticeVersion{1, 1};
        constexpr std::uint64_t PointDistanceTest = LatticeProvider + 6;
        constexpr std::uint64_t DistanceRadius = LatticeProvider + 7;
        constexpr std::uint64_t DistancePreferNearer = LatticeProvider + 8;

        /** @brief Native origin shared by the inert registration and its exact kernels. */
        QueryDescriptorOrigin BuiltinOrigin() {
            return {.provider = QueryProviderId::Create(LatticeProvider).Value(), .version = LatticeVersion.minimum};
        }

        /** @brief Uses the same numeric admission for authored settings and actual candidate generation. */
        bool ValidLattice(const QueryPointLattice &settings) {
            return settings.stepMillimeters > 0 && settings.outerSteps <= MaximumQueryLatticeOuterSteps &&
                   settings.innerSteps.value_or(0) <= settings.outerSteps;
        }

        /** @brief Projects an admitted unsigned source property without narrowing unchecked values. */
        std::optional<std::uint32_t> UnsignedProperty(const QueryPlanStage &stage, const QueryPropertyId id) {
            const auto entry = std::ranges::find(stage.properties, id, &QueryPropertyValue::id);
            if (entry == stage.properties.end())
                return {};
            const auto *value = std::get_if<std::uint64_t>(&entry->value);
            if (!value || *value > std::numeric_limits<std::uint32_t>::max())
                return {};
            return static_cast<std::uint32_t>(*value);
        }

        /** @brief Reads the exact authored property identities retained in the compiled plan. */
        std::optional<QueryPointLattice> PlanLattice(const QueryPlanStage &stage) {
            const auto step = UnsignedProperty(stage, QueryPropertyId::Create(StepMillimeters).Value());
            const auto outer = UnsignedProperty(stage, QueryPropertyId::Create(OuterSteps).Value());
            if (!step || !outer)
                return {};
            const auto inner = QueryPropertyId::Create(InnerSteps).Value();
            if (std::ranges::find(stage.properties, inner, &QueryPropertyValue::id) == stage.properties.end())
                return QueryPointLattice{*step, *outer};
            const auto innerValue = UnsignedProperty(stage, inner);
            if (!innerValue)
                return {};
            return QueryPointLattice{*step, *outer, *innerValue};
        }

        /** @brief Adds a bounded lattice offset while preserving the full global millimeter range. */
        [[nodiscard]] std::optional<std::int64_t> Offset(const std::int64_t base, const std::int64_t delta) {
            if (delta > 0 && base > std::numeric_limits<std::int64_t>::max() - delta)
                return std::nullopt;
            if (delta < 0 && base < std::numeric_limits<std::int64_t>::min() - delta)
                return std::nullopt;
            return base + delta;
        }

        /** @brief Evaluates a bounded square/annulus in canonical lattice order, with an optional publication destination. */
        [[nodiscard]] QueryGenerationResult SampleLattice(const Math::WorldCoordinate64 center, const QueryPointLattice &lattice,
                                                          const QueryGeneratorProvenance &origin,
                                                          const std::span<QueryGeneratedItem> destination) {
            QueryGenerationResult result{.provenance = origin};
            const auto point = center.Millimeters();
            const auto outer = static_cast<std::int64_t>(lattice.outerSteps);
            const auto inner = static_cast<std::int64_t>(lattice.innerSteps.value_or(0));
            for (auto z = -outer; z <= outer; ++z) {
                for (auto x = -outer; x <= outer; ++x) {
                    const auto squared = x * x + z * z;
                    if (lattice.innerSteps && (squared < inner * inner || squared > outer * outer))
                        continue;
                    const auto horizontal = Offset(point[0], x * lattice.stepMillimeters);
                    const auto depth = Offset(point[2], z * lattice.stepMillimeters);
                    if (!horizontal || !depth) {
                        ++result.invalidCoordinateCount;
                        continue;
                    }
                    if (!destination.empty())
                        destination[result.itemCount] = Math::WorldCoordinate64::FromMillimeters(*horizontal, point[1], *depth);
                    ++result.itemCount;
                }
            }
            return result;
        }

        /** @brief Uses an explicit canonical order rather than discovery or sense order. */
        [[nodiscard]] auto SourceOrder(const PerceptionSourceRef &source) noexcept {
            return std::tuple{source.slot, source.generation};
        }
    }  // namespace

    /** @copydoc QueryGeneratorOutput::IsValid */
    bool QueryGeneratorOutput::IsValid() const noexcept {
        return provenance.stage.IsValid() && provenance.generator.IsValid() && provenance.provider.IsValid() && provenance.revision > 0 &&
               maximumItems > 0 && maximumItems <= EnvironmentQuerySchemaLimits::MaximumResults && maximumItems <= items.size();
    }

    /** @copydoc GenerateQueryLattice */
    Result<QueryGenerationResult> GenerateQueryLattice(const QueryContextSnapshot &contexts, const QueryContextId center,
                                                       const QueryPointLattice &lattice, const QueryGeneratorOutput &output) {
        if (!output.IsValid() || !ValidLattice(lattice))
            return Generation::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        if (contexts.Revision() != output.provenance.revision)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto *context = contexts.Find(center);
        if (!context)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryContextMissing));
        const auto *position = std::get_if<Math::WorldCoordinate64>(&context->value);
        if (!position)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        const auto measured = SampleLattice(*position, lattice, output.provenance, {});
        if (measured.itemCount > output.maximumItems)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        return Generation::Success(SampleLattice(*position, lattice, output.provenance, output.items.first(output.maximumItems)));
    }

    /** @copydoc GenerateQueryPoints */
    Result<QueryGenerationResult> GenerateQueryPoints(const std::span<const Math::WorldCoordinate64> points,
                                                      const QueryGeneratorOutput &output) {
        if (!output.IsValid())
            return Generation::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        if (points.size() > output.maximumItems)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        std::ranges::copy(points, output.items.begin());
        return Generation::Success({.provenance = output.provenance, .itemCount = points.size()});
    }

    /** @copydoc CaptureBuiltinQuerySchema */
    Result<QuerySchemaRegistry> CaptureBuiltinQuerySchema() {
        const auto origin = BuiltinOrigin();
        const auto item = QueryItemTypeId::Create(PointItem).Value();
        const std::array items{QueryItemTypeDescriptor{.id = item, .origin = origin, .kind = QueryItemKind::Point}};
        const auto contexts = BuiltinQueryContextDescriptors();
        const std::array generators{
            QueryGeneratorDescriptor{.id = QueryGeneratorId::Create(LatticeGenerator).Value(),
                                     .origin = origin,
                                     .outputItemType = item,
                                     .outputItemVersion = LatticeVersion,
                                     .contexts = {{BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation), LatticeVersion}},
                                     .properties = {{QueryPropertyId::Create(StepMillimeters).Value(), QueryPropertyKind::Unsigned64, true},
                                                    {QueryPropertyId::Create(OuterSteps).Value(), QueryPropertyKind::Unsigned64, true},
                                                    {QueryPropertyId::Create(InnerSteps).Value(), QueryPropertyKind::Unsigned64, false}}}};
        const std::array tests{QueryTestDescriptor{
            .id = QueryTestId::Create(PointDistanceTest).Value(),
            .origin = origin,
            .inputItemType = item,
            .inputItemVersion = LatticeVersion,
            .contexts = {{BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation), LatticeVersion}},
            .properties = {{QueryPropertyId::Create(DistanceRadius).Value(), QueryPropertyKind::Unsigned64, true},
                           {QueryPropertyId::Create(DistancePreferNearer).Value(), QueryPropertyKind::Boolean, true}},
        }};
        return QuerySchemaRegistry::Capture({items, contexts, generators, tests});
    }

    /** @copydoc CreateBuiltinLatticeQuery */
    Result<EnvironmentQueryAsset> CreateBuiltinLatticeQuery(QueryId query, QueryStageId stage, QueryResultSchemaId result,
                                                            std::string displayName, const QueryPointLattice &lattice,
                                                            std::uint32_t maximumResults) {
        if (!ValidLattice(lattice))
            return Result<EnvironmentQueryAsset>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        QueryAssetStage generator{.id = stage,
                                  .kind = QueryStageKind::Generator,
                                  .generator = QueryGeneratorId::Create(LatticeGenerator).Value(),
                                  .descriptorVersion = LatticeVersion,
                                  .properties = {{QueryPropertyId::Create(StepMillimeters).Value(), std::uint64_t{lattice.stepMillimeters}},
                                                 {QueryPropertyId::Create(OuterSteps).Value(), std::uint64_t{lattice.outerSteps}}}};
        if (lattice.innerSteps)
            generator.properties.push_back({QueryPropertyId::Create(InnerSteps).Value(), std::uint64_t{*lattice.innerSteps}});
        const QueryResultSchema results{.id = result,
                                        .version = LatticeVersion.minimum,
                                        .itemType = QueryItemTypeId::Create(PointItem).Value(),
                                        .itemVersion = LatticeVersion,
                                        .maximumResults = maximumResults};
        return EnvironmentQueryAsset::Capture(
            {.id = query, .displayName = std::move(displayName), .result = results, .stages = {std::move(generator)}});
    }

    /** @copydoc AddBuiltinPointDistanceTest */
    Result<EnvironmentQueryAsset> AddBuiltinPointDistanceTest(const EnvironmentQueryAsset &lattice, QueryStageId stage,
                                                              QueryPointDistanceTest settings) {
        if (!stage.IsValid() || settings.radiusMillimeters == 0 || lattice.Stages().size() != 1 ||
            lattice.Stages().front().generator != QueryGeneratorId::Create(LatticeGenerator).Value() ||
            lattice.ResultSchema().itemType != QueryItemTypeId::Create(PointItem).Value())
            return Result<EnvironmentQueryAsset>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        const auto &generator = lattice.Stages().front();
        QueryAssetStage test{
            .id = stage,
            .kind = QueryStageKind::Test,
            .executionOrder = 1,
            .test = QueryTestId::Create(PointDistanceTest).Value(),
            .descriptorVersion = LatticeVersion,
            .properties = {{QueryPropertyId::Create(DistanceRadius).Value(), static_cast<std::uint64_t>(settings.radiusMillimeters)},
                           {QueryPropertyId::Create(DistancePreferNearer).Value(), settings.preferNearer}},
        };
        return EnvironmentQueryAsset::Capture({.id = lattice.Id(),
                                               .displayName = lattice.DisplayName(),
                                               .result = lattice.ResultSchema(),
                                               .stages = {generator, std::move(test)}});
    }

    /** @copydoc BuiltinPointDistanceQuery::Capture */
    Result<BuiltinPointDistanceQuery> BuiltinPointDistanceQuery::Capture(const QueryPlanStage &stage, const QuerySchemaRegistry &registry) {
        using Captured = Result<BuiltinPointDistanceQuery>;
        const auto *descriptor = registry.Find(stage.test);
        if (!stage.id.IsValid() || stage.kind != QueryStageKind::Test || stage.test != QueryTestId::Create(PointDistanceTest).Value() ||
            !descriptor || descriptor->origin != BuiltinOrigin())
            return Captured::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        const auto radius = UnsignedProperty(stage, QueryPropertyId::Create(DistanceRadius).Value());
        const auto scoring =
            std::ranges::find(stage.properties, QueryPropertyId::Create(DistancePreferNearer).Value(), &QueryPropertyValue::id);
        if (!radius || *radius == 0 || scoring == stage.properties.end() || !std::holds_alternative<bool>(scoring->value))
            return Captured::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        return Captured::Success(BuiltinPointDistanceQuery{stage.id, {*radius, std::get<bool>(scoring->value)}});
    }

    /** @copydoc BuiltinPointDistanceQuery::Rule */
    QueryScoreRule BuiltinPointDistanceQuery::Rule() const {
        QueryScoreRule rule{.stage = stage_, .requiredFilterValue = true};
        if (settings_.preferNearer)
            rule.score = QueryScoreRange{.maximum = settings_.radiusMillimeters / 1000.0, .direction = QueryScoreDirection::Lower};
        return rule;
    }

    /** @copydoc BuiltinPointDistanceQuery::Evaluate */
    Result<QueryTestSample> BuiltinPointDistanceQuery::Evaluate(Math::WorldCoordinate64 center, Math::WorldCoordinate64 point) const {
        auto sample = EvaluateQueryDistance(stage_, QueryDistanceMetric::Euclidean, center, point);
        if (sample.HasValue())
            sample.Value().filter = *sample.Value().metric <= settings_.radiusMillimeters / 1000.0;
        return sample;
    }

    /** @copydoc GenerateBuiltinQueryCandidates */
    Result<QueryGenerationResult> GenerateBuiltinQueryCandidates(const EnvironmentQueryPlan &plan, const QueryContextSnapshot &contexts,
                                                                 const QueryGeneratorOutput &output) {
        const auto stage = std::ranges::find(plan.Stages(), output.provenance.stage, &QueryPlanStage::id);
        const auto generator = QueryGeneratorId::Create(LatticeGenerator).Value();
        if (stage == plan.Stages().end() || stage->kind != QueryStageKind::Generator || stage->generator != generator ||
            output.provenance.generator != generator || output.provenance.provider != QueryProviderId::Create(LatticeProvider).Value() ||
            plan.ResultSchema().itemType != QueryItemTypeId::Create(PointItem).Value())
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        if (contexts.Query() != plan.Id())
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto lattice = PlanLattice(*stage);
        if (!lattice)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        return GenerateQueryLattice(contexts, BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation), *lattice, output);
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
        const auto &memory = current.Value().memory;
        std::array<PerceptionSourceRef, MaximumPerceptionMemoryEntries> records{};
        auto candidates = std::span{records}.first(memory.count);
        std::ranges::transform(std::span{memory.entries}.first(memory.count), candidates.begin(), [](const PerceivedStimulus &entry) {
            return entry.key.source;
        });
        std::ranges::sort(candidates, {}, SourceOrder);
        const auto duplicates = std::ranges::unique(candidates);
        const auto count = static_cast<std::size_t>(duplicates.begin() - candidates.begin());
        if (count > output.maximumItems)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        std::ranges::transform(candidates.first(count), output.items.begin(), [&source](const PerceptionSourceRef &record) {
            return Runtime::EntityRef{source.scene.RuntimeId(), {record.slot, record.generation}};
        });
        return Generation::Success({.provenance = output.provenance, .itemCount = count});
    }
}  // namespace Horo::AI
