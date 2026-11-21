#include "Horo/AI/EnvironmentQueryGenerators.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryExecution.h"
#include "Horo/AI/EnvironmentQueryPerception.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <new>
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
        constexpr std::uint64_t DistanceScoreWeight = LatticeProvider + 9;
        constexpr std::uint64_t PointCenter = LatticeProvider + 10;
        constexpr std::uint64_t PointVisibilityTest = LatticeProvider + 11;
        constexpr std::uint64_t VisibilityChannel = LatticeProvider + 12;

        constexpr std::array NativePointCenters{BuiltinQueryContext::QuerierLocation, BuiltinQueryContext::WorldOrigin,
                                                BuiltinQueryContext::TargetLocation};

        /** @brief Supplies the capturable native location alternatives for a point stage. */
        QueryContextChoice PointCenterChoice(std::span<const QueryContextDescriptor> additional) {
            QueryContextChoice choice{.property = QueryPropertyId::Create(PointCenter).Value()};
            choice.options.reserve(NativePointCenters.size() + additional.size());
            for (const auto kind : NativePointCenters)
                choice.options.emplace_back(BuiltinQueryContextId(kind), LatticeVersion);
            for (const auto &descriptor : additional)
                choice.options.emplace_back(descriptor.id, QueryVersionRange{descriptor.origin.version, descriptor.origin.version});
            return choice;
        }

        /** @brief Retains the original serialized shape for the default native location. */
        void EncodePointCenter(std::vector<QueryPropertyValue> &properties, QueryContextId center) {
            if (center.IsValid() && center != BuiltinQueryContextId(NativePointCenters.front()))
                properties.emplace_back(QueryPropertyId::Create(PointCenter).Value(), center.Value());
        }

        /** @brief Projects the installed choice list into editor-owned settings. */
        std::vector<QueryPointContextOption> PointCenterOptions(const std::optional<QueryContextChoice> &choice,
                                                                const QuerySchemaRegistry &registry) {
            std::vector<QueryPointContextOption> options;
            if (choice.has_value()) {
                options.reserve(choice->options.size());
                for (const auto &option : choice->options)
                    options.emplace_back(option.id, registry.Find(option.id)->displayName);
            }
            return options;
        }

        /** @brief Encodes only the native editable controls for one distance stage. */
        std::vector<QueryPropertyValue> DistanceProperties(const QueryPointDistanceTest settings) {
            std::vector<QueryPropertyValue> properties{{QueryPropertyId::Create(DistanceRadius).Value(),
                                                        std::uint64_t{settings.radiusMillimeters}},
                                                       {QueryPropertyId::Create(DistancePreferNearer).Value(), settings.preferNearer}};
            if (settings.scoreWeight != QueryPointDistanceTest{}.scoreWeight)
                properties.emplace_back(QueryPropertyId::Create(DistanceScoreWeight).Value(), std::uint64_t{settings.scoreWeight});
            EncodePointCenter(properties, settings.center);
            return properties;
        }

        /** @brief Recognizes the source shape admitted by native point-lattice authoring. */
        bool HasBuiltinLatticeStages(const EnvironmentQueryAsset &asset) {
            const auto stages = asset.Stages();
            const auto first = std::ranges::min_element(stages, {}, &QueryAssetStage::executionOrder);
            const auto generator = QueryGeneratorId::Create(LatticeGenerator).Value();
            if (first == stages.end() || first->kind != QueryStageKind::Generator || first->generator != generator ||
                asset.ResultSchema().itemType != QueryItemTypeId::Create(PointItem).Value())
                return false;
            const auto test = QueryTestId::Create(PointDistanceTest).Value();
            return std::ranges::all_of(stages, [first, test](const QueryAssetStage &stage) {
                return &stage == &*first || (stage.kind == QueryStageKind::Test && stage.test == test);
            });
        }

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
        std::optional<std::uint32_t> UnsignedProperty(const QueryPlanStage &stage, const QueryPropertyId id,
                                                      const std::optional<std::uint32_t> whenAbsent = {}) {
            const auto entry = std::ranges::find(stage.properties, id, &QueryPropertyValue::id);
            if (entry == stage.properties.end())
                return whenAbsent;
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
                return QueryPointLattice{*step, *outer, {}, stage.selectedContext};
            const auto innerValue = UnsignedProperty(stage, inner);
            if (!innerValue)
                return {};
            return QueryPointLattice{*step, *outer, *innerValue, stage.selectedContext};
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

        /** @brief Generates only after proving the complete batch fits its destination. */
        Generation GenerateLatticeAtPoint(const Math::WorldCoordinate64 &point, const QueryPointLattice &lattice,
                                          const QueryGeneratorOutput &output) {
            if (const auto measured = SampleLattice(point, lattice, output.provenance, {}); measured.itemCount > output.maximumItems)
                return Generation::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
            return Generation::Success(SampleLattice(point, lattice, output.provenance, output.items.first(output.maximumItems)));
        }

        /** @brief Holds one typed native test and the exact location fixed at owner admission. */
        struct PointOwnerTest final {
            QueryStageId stage;
            BuiltinPointDistanceQuery kernel;
            Math::WorldCoordinate64 center;
        };

        /** @brief Preserves typed kernel failures even when a diagnostic cannot allocate. */
        template <typename Value> Result<Value> OwnerFailure(const ErrorCodeDescriptor &error) noexcept {
            return Result<Value>::Failure({.code = error.code, .domain = error.domain, .severity = error.defaultSeverity});
        }

        /** @brief Evaluates detached native point kernels on the simulation owner under declared work bounds. */
        class PointOwnerSource final : public IOwnerQueryExecutionSource {
        public:
            PointOwnerSource(const Runtime::RuntimeSceneView &scene, const QueryPointLattice &lattice,
                             const Math::WorldCoordinate64 &center, std::vector<PointOwnerTest> tests, QueryOwnerWorkLimits limits)
                : IOwnerQueryExecutionSource(limits), scene_(scene), lattice_(lattice), center_(center), tests_(std::move(tests)) {}

            [[nodiscard]] bool IsCurrent(const QueryExecutionInputs &inputs) const noexcept override {
                return inputs.contexts->MatchesScene(scene_);
            }

        private:
            Result<QueryGenerationResult> GenerateAtOwner(const QueryExecutionInputs &, const QueryGeneratorOutput &output,
                                                          AiWorkBudget &budget) noexcept override {
                if (!budget.Consume(WorkLimits().generationUnits))
                    return OwnerFailure<QueryGenerationResult>(AIErrors::SchedulerBudgetExhausted);
                try {
                    return GenerateLatticeAtPoint(center_, lattice_, output);
                } catch (const std::bad_alloc &) {
                    return OwnerFailure<QueryGenerationResult>(AIErrors::EnvironmentQueryLimitExceeded);
                }
            }

            Result<QueryTestSample> TestAtOwner(const QueryExecutionInputs &, QueryStageId stage, const QueryGeneratedItem &item,
                                                AiWorkBudget &budget) noexcept override {
                if (!budget.Consume(WorkLimits().testUnits))
                    return OwnerFailure<QueryTestSample>(AIErrors::SchedulerBudgetExhausted);
                const auto test = std::ranges::lower_bound(tests_, stage, {}, &PointOwnerTest::stage);
                const auto *point = std::get_if<Math::WorldCoordinate64>(&item);
                if (test == tests_.end() || test->stage != stage || !point)
                    return OwnerFailure<QueryTestSample>(AIErrors::EnvironmentQuerySchemaInvalid);
                try {
                    return test->kernel.Evaluate(test->center, *point);
                } catch (const std::bad_alloc &) {
                    return OwnerFailure<QueryTestSample>(AIErrors::EnvironmentQueryProviderFailed);
                }
            }

            Runtime::RuntimeSceneView scene_;
            QueryPointLattice lattice_;
            Math::WorldCoordinate64 center_;
            std::vector<PointOwnerTest> tests_;
        };

        /** @brief Extracts an owned canonical location from the exact native context slot. */
        Result<Math::WorldCoordinate64> ReadPointContext(const QueryContextSnapshot &contexts, QueryContextId id) {
            using Point = Result<Math::WorldCoordinate64>;
            const auto *context = contexts.Find(id);
            if (!context)
                return Point::Failure(MakeError(AIErrors::EnvironmentQueryContextMissing));
            if (const auto *point = std::get_if<Math::WorldCoordinate64>(&context->value))
                return Point::Success(*point);
            return Point::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        }

        /** @brief Retains the admitted lattice, selected location and exact generator publication identity. */
        struct PointOwnerGenerator final {
            QueryPointLattice lattice;
            Math::WorldCoordinate64 center;
            QueryGeneratorProvenance provenance;
        };

        /** @brief Resolves the generator descriptor and its pinned location before owner execution. */
        Result<PointOwnerGenerator> CapturePointOwnerGenerator(const QueryPlanStage &stage, QueryItemTypeId itemType,
                                                               const QueryContextSnapshot &contexts, const QuerySchemaRegistry &registry) {
            using Generator = Result<PointOwnerGenerator>;
            const auto *descriptor = registry.Find(stage.generator);
            const auto *item = registry.Find(itemType);
            if (!descriptor || descriptor->origin != BuiltinOrigin() ||
                descriptor->id != QueryGeneratorId::Create(LatticeGenerator).Value() || descriptor->outputItemType != itemType)
                return Generator::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            if (!item || itemType != QueryItemTypeId::Create(PointItem).Value() || item->kind != QueryItemKind::Point)
                return Generator::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            const auto lattice = PlanLattice(stage);
            if (!lattice.has_value() || !ValidLattice(*lattice))
                return Generator::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            const auto center = ReadPointContext(contexts, lattice->center);
            if (center.HasError())
                return Generator::Failure(center.ErrorValue());
            return Generator::Success(
                {*lattice, center.Value(), {stage.id, descriptor->id, descriptor->origin.provider, contexts.Revision()}});
        }

        /** @brief Validated native distance kernels and their scoring rules before admission. */
        struct PointOwnerTests final {
            std::vector<PointOwnerTest> kernels;
            std::vector<QueryScoreRule> rules;
        };

        /** @brief Pins the location for each authored test and keeps its independent ranking rule. */
        Result<PointOwnerTests> CapturePointOwnerTests(std::span<const QueryPlanStage> stages, const QueryContextSnapshot &contexts,
                                                       const QuerySchemaRegistry &registry) {
            using Tests = Result<PointOwnerTests>;
            PointOwnerTests tests;
            tests.kernels.reserve(stages.size());
            tests.rules.reserve(stages.size());
            for (const auto &stage : stages) {
                auto kernel = BuiltinPointDistanceQuery::Capture(stage, registry);
                if (kernel.HasError())
                    return Tests::Failure(kernel.ErrorValue());
                const auto center = ReadPointContext(contexts, kernel.Value().Settings().center);
                if (center.HasError())
                    return Tests::Failure(center.ErrorValue());
                tests.rules.push_back(kernel.Value().Rule());
                tests.kernels.emplace_back(stage.id, std::move(kernel).Value(), center.Value());
            }
            std::ranges::sort(tests.kernels, {}, &PointOwnerTest::stage);
            return Tests::Success(std::move(tests));
        }

        /** @brief Extension contexts have no dependency footprint in the native point-query adapter. */
        bool HasOnlyBuiltinPointContexts(const EnvironmentQueryPlan &plan) {
            return std::ranges::all_of(plan.RequiredContexts(), [](const auto &context) {
                return std::ranges::any_of(NativePointCenters, [&](const auto kind) {
                    return BuiltinQueryContextId(kind) == context.id;
                });
            });
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
        const auto position = ReadPointContext(contexts, center);
        if (position.HasError())
            return Generation::Failure(position.ErrorValue());
        return GenerateLatticeAtPoint(position.Value(), lattice, output);
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
        return CaptureBuiltinQuerySchema({});
    }

    /** @copydoc CaptureBuiltinQuerySchema(std::span<const QueryContextDescriptor>) */
    Result<QuerySchemaRegistry> CaptureBuiltinQuerySchema(std::span<const QueryContextDescriptor> locationContexts) {
        using enum QueryPropertyKind;
        if (locationContexts.size() > EnvironmentQuerySchemaLimits::DescriptorContexts - NativePointCenters.size())
            return Result<QuerySchemaRegistry>::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        const auto origin = BuiltinOrigin();
        const auto item = QueryItemTypeId::Create(PointItem).Value();
        auto perception = DescribeBuiltinPerceptionQuery();
        const std::array items{QueryItemTypeDescriptor{.id = item, .origin = origin, .kind = QueryItemKind::Point}, perception.item};
        const auto builtin = BuiltinQueryContextDescriptors();
        std::vector<QueryContextDescriptor> contexts(builtin.begin(), builtin.end());
        contexts.insert(contexts.end(), locationContexts.begin(), locationContexts.end());
        const auto centers = PointCenterChoice(locationContexts);
        const std::array generators{QueryGeneratorDescriptor{.id = QueryGeneratorId::Create(LatticeGenerator).Value(),
                                                             .origin = origin,
                                                             .outputItemType = item,
                                                             .outputItemVersion = LatticeVersion,
                                                             .properties =
                                                                 {{QueryPropertyId::Create(StepMillimeters).Value(), Unsigned64, true},
                                                                  {QueryPropertyId::Create(OuterSteps).Value(), Unsigned64, true},
                                                                  {QueryPropertyId::Create(InnerSteps).Value(), Unsigned64, false},
                                                                  {QueryPropertyId::Create(PointCenter).Value(), Unsigned64, false}},
                                                             .contextChoice = centers},
                                    std::move(perception.generator)};
        const std::array tests{QueryTestDescriptor{
                                   .id = QueryTestId::Create(PointDistanceTest).Value(),
                                   .origin = origin,
                                   .inputItemType = item,
                                   .inputItemVersion = LatticeVersion,
                                   .properties = {{QueryPropertyId::Create(DistanceRadius).Value(), Unsigned64, true},
                                                  {QueryPropertyId::Create(DistancePreferNearer).Value(), Boolean, true},
                                                  {QueryPropertyId::Create(DistanceScoreWeight).Value(), Unsigned64, false},
                                                  {QueryPropertyId::Create(PointCenter).Value(), Unsigned64, false}},
                                   .contextChoice = centers,
                               },
                               QueryTestDescriptor{
                                   .id = QueryTestId::Create(PointVisibilityTest).Value(),
                                   .origin = origin,
                                   .inputItemType = item,
                                   .inputItemVersion = LatticeVersion,
                                   .contexts = {{BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation), LatticeVersion}},
                                   .properties = {{QueryPropertyId::Create(VisibilityChannel).Value(), CanonicalBytes, true, 16}},
                               },
                               std::move(perception.test)};
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
        EncodePointCenter(generator.properties, lattice.center);
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
        using Edited = Result<EnvironmentQueryAsset>;
        if (!stage.IsValid() || settings.radiusMillimeters == 0 || settings.scoreWeight == 0 || !HasBuiltinLatticeStages(lattice))
            return Edited::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        const auto stages = lattice.Stages();
        const auto last = std::ranges::max_element(stages, {}, &QueryAssetStage::executionOrder);
        if (stages.size() == EnvironmentQuerySchemaLimits::Stages || last->executionOrder == std::numeric_limits<std::uint32_t>::max())
            return Edited::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        QueryAssetSource edited{.id = lattice.Id(),
                                .displayName = lattice.DisplayName(),
                                .result = lattice.ResultSchema(),
                                .stages = {stages.begin(), stages.end()}};
        edited.stages.push_back({
            .id = stage,
            .kind = QueryStageKind::Test,
            .executionOrder = last->executionOrder + 1,
            .test = QueryTestId::Create(PointDistanceTest).Value(),
            .descriptorVersion = LatticeVersion,
            .properties = DistanceProperties(settings),
        });
        return EnvironmentQueryAsset::Capture(edited);
    }

    /** @copydoc AddBuiltinPointVisibilityTest */
    Result<EnvironmentQueryAsset> AddBuiltinPointVisibilityTest(const EnvironmentQueryAsset &lattice, QueryStageId stage,
                                                                std::array<std::uint8_t, 16> channel) {
        using Asset = Result<EnvironmentQueryAsset>;
        if (!stage.IsValid() || lattice.Stages().size() != 1 || !HasBuiltinLatticeStages(lattice) ||
            std::ranges::all_of(channel, [](auto byte) {
            return byte == 0;
        }))
            return Asset::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        const auto generator = lattice.Stages().front();
        if (generator.executionOrder == std::numeric_limits<std::uint32_t>::max())
            return Asset::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        const auto bytes = std::as_bytes(std::span{channel});
        return EnvironmentQueryAsset::Capture({
            .id = lattice.Id(),
            .displayName = lattice.DisplayName(),
            .result = lattice.ResultSchema(),
            .stages = {generator,
                       {
                           .id = stage,
                           .kind = QueryStageKind::Test,
                           .executionOrder = generator.executionOrder + 1,
                           .test = QueryTestId::Create(PointVisibilityTest).Value(),
                           .descriptorVersion = LatticeVersion,
                           .properties = {{QueryPropertyId::Create(VisibilityChannel).Value(),
                                           std::vector<std::byte>{bytes.begin(), bytes.end()}}},
                       }},
        });
    }

    /** @copydoc UsesBuiltinPointVisibility */
    bool UsesBuiltinPointVisibility(const EnvironmentQueryPlan &plan) noexcept {
        return std::ranges::any_of(plan.Stages(), [](const QueryPlanStage &stage) {
            return stage.test == QueryTestId::Create(PointVisibilityTest).Value();
        });
    }

    /** @copydoc CaptureBuiltinPointVisibility */
    Result<BuiltinPointVisibilityPlan> CaptureBuiltinPointVisibility(const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry,
                                                                     const QueryContextSnapshot &contexts) {
        using Captured = Result<BuiltinPointVisibilityPlan>;
        const auto stages = plan.Stages();
        if (stages.size() != 2 || contexts.Query() != plan.Id() || stages.back().test != QueryTestId::Create(PointVisibilityTest).Value())
            return Captured::Failure(MakeError(AIErrors::EnvironmentQueryStageUnsupported));

        const auto *test = registry.Find(stages.back().test);
        if (!test || test->origin != BuiltinOrigin())
            return Captured::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        const auto generator = CapturePointOwnerGenerator(stages.front(), plan.ResultSchema().itemType, contexts, registry);
        if (generator.HasError())
            return Captured::Failure(generator.ErrorValue());
        const auto property =
            std::ranges::find(stages.back().properties, QueryPropertyId::Create(VisibilityChannel).Value(), &QueryPropertyValue::id);
        const auto *bytes = property == stages.back().properties.end() ? nullptr : std::get_if<std::vector<std::byte>>(&property->value);
        std::array<std::uint8_t, 16> channel{};
        if (!bytes || bytes->size() != channel.size() || std::ranges::all_of(*bytes, [](std::byte value) {
            return value == std::byte{};
        }))
            return Captured::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        std::ranges::transform(*bytes, channel.begin(), [](std::byte value) {
            return std::to_integer<std::uint8_t>(value);
        });
        return Captured::Success({generator.Value().lattice, generator.Value().provenance, stages.back().id, channel});
    }

    /** @copydoc RemoveBuiltinPointDistanceTest */
    Result<EnvironmentQueryAsset> RemoveBuiltinPointDistanceTest(const EnvironmentQueryAsset &lattice, QueryStageId stage) {
        const auto stages = lattice.Stages();
        const auto test = std::ranges::find(stages, stage, &QueryAssetStage::id);
        if (test == stages.end() || test->kind != QueryStageKind::Test || test->test != QueryTestId::Create(PointDistanceTest).Value())
            return Result<EnvironmentQueryAsset>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        QueryAssetSource edited{.id = lattice.Id(),
                                .displayName = lattice.DisplayName(),
                                .result = lattice.ResultSchema(),
                                .stages = {stages.begin(), stages.end()}};
        edited.stages.erase(edited.stages.begin() + (test - stages.begin()));
        return EnvironmentQueryAsset::Capture(edited);
    }

    /** @copydoc ReadBuiltinPointQuerySettings */
    Result<BuiltinPointQuerySettings> ReadBuiltinPointQuerySettings(const EnvironmentQueryAsset &asset,
                                                                    const QuerySchemaRegistry &registry) {
        using Settings = Result<BuiltinPointQuerySettings>;
        const auto plan = EnvironmentQueryPlan::Compile(asset, registry);
        if (plan.HasError())
            return Settings::Failure(plan.ErrorValue());
        const auto stages = plan.Value().Stages();
        const auto *generator = registry.Find(stages.front().generator);
        if (!generator || generator->origin != BuiltinOrigin() || generator->id != QueryGeneratorId::Create(LatticeGenerator).Value())
            return Settings::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        const auto lattice = PlanLattice(stages.front());
        if (!lattice || !ValidLattice(*lattice))
            return Settings::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        BuiltinPointQuerySettings settings{*lattice, asset.ResultSchema().maximumResults};
        settings.latticeCenters = PointCenterOptions(generator->contextChoice, registry);
        settings.distanceTests.reserve(stages.size() - 1);
        for (const auto &stage : stages.subspan(1)) {
            const auto distance = BuiltinPointDistanceQuery::Capture(stage, registry);
            if (distance.HasError())
                return Settings::Failure(distance.ErrorValue());
            settings.distanceTests.emplace_back(stage.id, distance.Value().Settings(),
                                                PointCenterOptions(registry.Find(stage.test)->contextChoice, registry));
        }
        return Settings::Success(std::move(settings));
    }

    /** @copydoc EditBuiltinPointQuerySettings */
    Result<EnvironmentQueryAsset> EditBuiltinPointQuerySettings(const EnvironmentQueryAsset &asset, const QuerySchemaRegistry &registry,
                                                                const BuiltinPointQuerySettings &settings) {
        using Edited = Result<EnvironmentQueryAsset>;
        const auto previous = ReadBuiltinPointQuerySettings(asset, registry);
        if (previous.HasError())
            return Edited::Failure(previous.ErrorValue());
        constexpr auto stageId = &QueryPointDistanceStageSettings::stage;
        if (!std::ranges::equal(previous.Value().distanceTests, settings.distanceTests, {}, stageId, stageId))
            return Edited::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));

        const auto generator = std::ranges::find(asset.Stages(), QueryStageKind::Generator, &QueryAssetStage::kind);
        auto generated = CreateBuiltinLatticeQuery(asset.Id(), generator->id, asset.ResultSchema().id, asset.DisplayName(),
                                                   settings.lattice, settings.maximumResults);
        if (generated.HasError())
            return generated;
        QueryAssetSource edited{.id = asset.Id(),
                                .displayName = asset.DisplayName(),
                                .result = asset.ResultSchema(),
                                .stages = {asset.Stages().begin(), asset.Stages().end()}};
        edited.result.maximumResults = settings.maximumResults;
        for (auto &stage : edited.stages) {
            if (stage.kind == QueryStageKind::Generator) {
                stage.properties = generated.Value().Stages().front().properties;
                continue;
            }
            const auto entry = std::ranges::find(settings.distanceTests, stage.id, stageId);
            stage.properties = DistanceProperties(entry->test);
        }
        auto captured = EnvironmentQueryAsset::Capture(edited);
        if (captured.HasError())
            return captured;
        const auto verified = ReadBuiltinPointQuerySettings(captured.Value(), registry);
        return verified.HasError() ? Edited::Failure(verified.ErrorValue()) : std::move(captured);
    }

    /** @copydoc BuiltinPointDistanceQuery::Capture */
    Result<BuiltinPointDistanceQuery> BuiltinPointDistanceQuery::Capture(const QueryPlanStage &stage, const QuerySchemaRegistry &registry) {
        using Captured = Result<BuiltinPointDistanceQuery>;
        const auto *descriptor = registry.Find(stage.test);
        if (!stage.id.IsValid() || stage.kind != QueryStageKind::Test || stage.test != QueryTestId::Create(PointDistanceTest).Value() ||
            !descriptor || descriptor->origin != BuiltinOrigin())
            return Captured::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        const auto radius = UnsignedProperty(stage, QueryPropertyId::Create(DistanceRadius).Value());
        const auto weight =
            UnsignedProperty(stage, QueryPropertyId::Create(DistanceScoreWeight).Value(), QueryPointDistanceTest{}.scoreWeight);
        const auto scoringProperty =
            std::ranges::find(stage.properties, QueryPropertyId::Create(DistancePreferNearer).Value(), &QueryPropertyValue::id);
        const auto *scoring = scoringProperty == stage.properties.end() ? nullptr : std::get_if<bool>(&scoringProperty->value);
        if (radius.value_or(0) == 0 || weight.value_or(0) == 0 || !scoring)
            return Captured::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        return Captured::Success(BuiltinPointDistanceQuery{stage.id, {*radius, *scoring, *weight, stage.selectedContext}});
    }

    /** @copydoc BuiltinPointDistanceQuery::Rule */
    QueryScoreRule BuiltinPointDistanceQuery::Rule() const {
        QueryScoreRule rule{.stage = stage_, .requiredFilterValue = true};
        if (settings_.preferNearer)
            rule.score = QueryScoreRange{.maximum = settings_.radiusMillimeters / 1000.0,
                                         .weight = static_cast<double>(settings_.scoreWeight),
                                         .direction = QueryScoreDirection::Lower};
        return rule;
    }

    /** @copydoc BuiltinPointDistanceQuery::Evaluate */
    Result<QueryTestSample> BuiltinPointDistanceQuery::Evaluate(Math::WorldCoordinate64 center, Math::WorldCoordinate64 point) const {
        auto evaluated = EvaluateQueryDistance(stage_, QueryDistanceMetric::Euclidean, center, point);
        if (evaluated.HasError())
            return evaluated;
        auto sample = std::move(evaluated).Value();
        sample.filter = *sample.metric <= settings_.radiusMillimeters / 1000.0;
        return Result<QueryTestSample>::Success(std::move(sample));
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
        return GenerateQueryLattice(contexts, lattice->center, *lattice, output);
    }

    /** @copydoc AdmitBuiltinPointQuery */
    Result<std::unique_ptr<QueryExecution>> AdmitBuiltinPointQuery(const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry,
                                                                   const Runtime::RuntimeSceneView &scene, QueryExecutionInputs inputs,
                                                                   QueryExecutionSettings settings, BuiltinPointQueryReuse reuse) {
        using Admission = Result<std::unique_ptr<QueryExecution>>;
        if (!inputs.contexts || inputs.contexts->Query() != plan.Id())
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        if (!inputs.contexts->MatchesScene(scene) || inputs.ownerCancellation.IsCancellationRequested())
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto stages = plan.Stages();
        if (stages.empty())
            return Admission::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        const auto generator = CapturePointOwnerGenerator(stages.front(), plan.ResultSchema().itemType, *inputs.contexts, registry);
        if (generator.HasError())
            return Admission::Failure(generator.ErrorValue());
        auto tests = CapturePointOwnerTests(stages.subspan(1), *inputs.contexts, registry);
        if (tests.HasError())
            return Admission::Failure(tests.ErrorValue());
        auto scoring = QueryScoringPlan::Capture(plan, registry, tests.Value().rules, QueryScoreTieBreak::StableIndex);
        if (scoring.HasError())
            return Admission::Failure(scoring.ErrorValue());

        const auto &captured = generator.Value();
        const std::size_t width = 2U * captured.lattice.outerSteps + 1U;
        // Reserve both complete lattice passes and one stage lookup plus distance evaluation.
        const QueryOwnerWorkLimits limits{2 * width * width, 1U + std::bit_width(tests.Value().kernels.size())};
        settings.generatorWorkUnits = limits.generationUnits;
        auto source =
            std::make_unique<PointOwnerSource>(scene, captured.lattice, captured.center, std::move(tests).Value().kernels, limits);
        const auto mode = reuse.cache ? QueryExecutionMode::AdaptiveRealtime : QueryExecutionMode::DeterministicWorkUnits;
        auto admitted = QueryExecution::AdmitOwner(std::move(inputs), captured.provenance, std::move(scoring).Value(), settings,
                                                   std::move(source), mode);
        if (reuse.cache && admitted.HasValue() && HasOnlyBuiltinPointContexts(plan))
            admitted.Value()->BindPointCache(scene, reuse);
        return admitted;
    }

}  // namespace Horo::AI
