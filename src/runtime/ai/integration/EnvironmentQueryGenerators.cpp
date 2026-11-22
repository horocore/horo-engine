#include "Horo/AI/EnvironmentQueryGenerators.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryExecution.h"
#include "Horo/AI/EnvironmentQueryGameplay.h"
#include "Horo/AI/EnvironmentQueryPerception.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
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
        constexpr std::uint64_t PointNavigationTest = LatticeProvider + 13;
        constexpr std::uint64_t NavigationMaximumScoredCost = LatticeProvider + 14;
        constexpr std::uint64_t NavigationScoreWeight = LatticeProvider + 15;
        constexpr std::uint64_t AuthoredPointGenerator = LatticeProvider + 16;
        constexpr std::uint64_t AuthoredPointFirst = LatticeProvider + 17;
        constexpr std::uint64_t NavigationSurfaceGenerator = AuthoredPointFirst + MaximumBuiltinAuthoredQueryPoints;
        constexpr std::uint64_t NavigationSurfaceRadius = NavigationSurfaceGenerator + 1;
        constexpr std::uint64_t NavigationSurfaceMaximum = NavigationSurfaceGenerator + 2;
        constexpr std::uint64_t DistanceScoreCurve = NavigationSurfaceMaximum + 1;
        constexpr std::uint64_t DistanceScoreDirection = NavigationSurfaceMaximum + 2;
        constexpr std::uint64_t DistanceScoreThreshold = NavigationSurfaceMaximum + 3;

        constexpr std::size_t PointAxisBytes = sizeof(std::int64_t);
        constexpr std::size_t PointBytes = 3 * PointAxisBytes;

        QueryPropertyId AuthoredPointProperty(const std::size_t index) {
            return QueryPropertyId::Create(AuthoredPointFirst + index).Value();
        }

        /** @brief Uses signed millimeters encoded as three little-endian axes, independent of the host ABI. */
        std::vector<std::byte> EncodeAuthoredPoint(const Math::WorldCoordinate64 point) {
            std::vector<std::byte> bytes;
            bytes.reserve(PointBytes);
            for (const auto axis : point.Millimeters()) {
                const auto bits = std::bit_cast<std::uint64_t>(axis);
                for (std::size_t offset = 0; offset < PointAxisBytes; ++offset)
                    bytes.push_back(static_cast<std::byte>((bits >> (offset * 8)) & 0xffU));
            }
            return bytes;
        }

        Math::WorldCoordinate64 DecodeAuthoredPoint(const std::span<const std::byte> bytes) noexcept {
            std::array<std::int64_t, 3> axes{};
            for (std::size_t index = 0; index < axes.size(); ++index) {
                std::uint64_t bits{};
                for (std::size_t offset = 0; offset < PointAxisBytes; ++offset)
                    bits |= std::to_integer<std::uint64_t>(bytes[index * PointAxisBytes + offset]) << (offset * 8);
                axes[index] = std::bit_cast<std::int64_t>(bits);
            }
            return Math::WorldCoordinate64::FromMillimeters(axes[0], axes[1], axes[2]);
        }

        using AuthoredPoints = BuiltinDetachedPointCandidates::Authored;

        /** @brief Decodes only a complete dense point sequence before any output can be written. */
        Result<AuthoredPoints> CaptureAuthoredPoints(const QueryPlanStage &stage) {
            using Capture = Result<AuthoredPoints>;
            if (stage.properties.empty() || stage.properties.size() > MaximumBuiltinAuthoredQueryPoints)
                return Capture::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            AuthoredPoints points{.count = stage.properties.size()};
            for (const auto &property : stage.properties) {
                const auto *bytes = std::get_if<std::vector<std::byte>>(&property.value);
                const auto index = property.id.Value() - AuthoredPointFirst;
                if (index >= points.count || !bytes || bytes->size() != PointBytes)
                    return Capture::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
                points.values[index] = DecodeAuthoredPoint(*bytes);
            }
            return Capture::Success(std::move(points));
        }

        QueryGeneratorDescriptor DescribeAuthoredPointGenerator(const QueryDescriptorOrigin origin, const QueryItemTypeId item) {
            QueryGeneratorDescriptor descriptor{.id = QueryGeneratorId::Create(AuthoredPointGenerator).Value(),
                                                .origin = origin,
                                                .outputItemType = item,
                                                .outputItemVersion = LatticeVersion};
            descriptor.properties.reserve(MaximumBuiltinAuthoredQueryPoints);
            for (std::size_t index = 0; index < MaximumBuiltinAuthoredQueryPoints; ++index)
                descriptor.properties.emplace_back(AuthoredPointProperty(index), QueryPropertyKind::CanonicalBytes, index == 0, PointBytes);
            return descriptor;
        }

        Result<EnvironmentQueryAsset> CapturePointQueryAsset(QueryId query, QueryResultSchemaId result, std::string displayName,
                                                             QueryAssetStage generator, const std::uint32_t maximumResults) {
            const QueryResultSchema results{.id = result,
                                            .version = LatticeVersion.minimum,
                                            .itemType = QueryItemTypeId::Create(PointItem).Value(),
                                            .itemVersion = LatticeVersion,
                                            .maximumResults = maximumResults};
            return EnvironmentQueryAsset::Capture(
                {.id = query, .displayName = std::move(displayName), .result = results, .stages = {std::move(generator)}});
        }

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

        bool ValidDistanceSettings(const QueryPointDistanceTest &settings) noexcept {
            return settings.radiusMillimeters > 0 && settings.scoreWeight > 0 && settings.response.IsValid() &&
                   (settings.preferNearer || settings.response == QueryDistanceScoreResponse{});
        }

        /** @brief Encodes only the native editable controls for one distance stage. */
        std::vector<QueryPropertyValue> DistanceProperties(const QueryPointDistanceTest settings) {
            std::vector<QueryPropertyValue> properties{{QueryPropertyId::Create(DistanceRadius).Value(),
                                                        std::uint64_t{settings.radiusMillimeters}},
                                                       {QueryPropertyId::Create(DistancePreferNearer).Value(), settings.preferNearer}};
            if (settings.scoreWeight != QueryPointDistanceTest{}.scoreWeight)
                properties.emplace_back(QueryPropertyId::Create(DistanceScoreWeight).Value(), std::uint64_t{settings.scoreWeight});
            const QueryDistanceScoreResponse defaults;
            if (settings.response.curve != defaults.curve)
                properties.emplace_back(QueryPropertyId::Create(DistanceScoreCurve).Value(),
                                        static_cast<std::uint64_t>(settings.response.curve));
            if (settings.response.direction != defaults.direction)
                properties.emplace_back(QueryPropertyId::Create(DistanceScoreDirection).Value(),
                                        static_cast<std::uint64_t>(settings.response.direction));
            if (settings.response.threshold != defaults.threshold)
                properties.emplace_back(QueryPropertyId::Create(DistanceScoreThreshold).Value(), settings.response.threshold);
            EncodePointCenter(properties, settings.center);
            return properties;
        }

        /** @brief Recognizes a built-in point generator followed by typed source tests. */
        bool HasBuiltinPointStages(const EnvironmentQueryAsset &asset) {
            const auto stages = asset.Stages();
            const auto first = std::ranges::min_element(stages, {}, &QueryAssetStage::executionOrder);
            if (first == stages.end() || first->kind != QueryStageKind::Generator ||
                asset.ResultSchema().itemType != QueryItemTypeId::Create(PointItem).Value())
                return false;
            const auto generator = first->generator.Value();
            if (generator != LatticeGenerator && generator != AuthoredPointGenerator && generator != NavigationSurfaceGenerator)
                return false;
            return std::ranges::all_of(stages, [first](const QueryAssetStage &stage) {
                return &stage == &*first || stage.kind == QueryStageKind::Test;
            });
        }

        /** @brief Appends one external provider stage while preserving the generator and any other provider stage. */
        Result<EnvironmentQueryAsset> AppendPointProviderTest(const EnvironmentQueryAsset &asset, QueryAssetStage stage) {
            using Edited = Result<EnvironmentQueryAsset>;
            const auto stages = asset.Stages();
            if (!stage.id.IsValid() || !HasBuiltinPointStages(asset))
                return Edited::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            if (stages.size() >= EnvironmentQuerySchemaLimits::Stages)
                return Edited::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
            if (std::ranges::find(stages, stage.test, &QueryAssetStage::test) != stages.end())
                return Edited::Failure(MakeError(AIErrors::EnvironmentQueryStageUnsupported));
            const auto last = std::ranges::max_element(stages, {}, &QueryAssetStage::executionOrder);
            if (last->executionOrder == std::numeric_limits<std::uint32_t>::max())
                return Edited::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
            stage.executionOrder = last->executionOrder + 1;
            QueryAssetSource edited{.id = asset.Id(),
                                    .displayName = asset.DisplayName(),
                                    .result = asset.ResultSchema(),
                                    .stages = {stages.begin(), stages.end()}};
            edited.stages.push_back(std::move(stage));
            return EnvironmentQueryAsset::Capture(edited);
        }

        const QueryPlanStage *FindPointProviderTest(const EnvironmentQueryPlan &plan, const std::uint64_t provider) {
            const auto id = QueryTestId::Create(provider).Value();
            const auto stages = plan.Stages();
            if (stages.size() < 2 || std::ranges::count(stages, id, &QueryPlanStage::test) != 1)
                return nullptr;
            return &*std::ranges::find(stages, id, &QueryPlanStage::test);
        }

        using VisibilityChannelId = std::array<std::uint8_t, 16>;

        std::optional<VisibilityChannelId> ReadVisibilityChannel(const QueryPlanStage &stage) {
            const auto property =
                std::ranges::find(stage.properties, QueryPropertyId::Create(VisibilityChannel).Value(), &QueryPropertyValue::id);
            const auto *bytes = property == stage.properties.end() ? nullptr : std::get_if<std::vector<std::byte>>(&property->value);
            VisibilityChannelId channel{};
            if (!bytes || bytes->size() != channel.size() || std::ranges::all_of(*bytes, [](auto value) {
                return value == std::byte{};
            }))
                return std::nullopt;
            std::ranges::transform(*bytes, channel.begin(), [](auto value) {
                return std::to_integer<std::uint8_t>(value);
            });
            return channel;
        }

        std::vector<QueryPropertyValue> VisibilityProperties(const VisibilityChannelId &channel) {
            const auto bytes = std::as_bytes(std::span{channel});
            return {{QueryPropertyId::Create(VisibilityChannel).Value(), std::vector<std::byte>{bytes.begin(), bytes.end()}}};
        }

        Result<std::optional<double>> ReadNavigationCost(const QueryPlanStage &stage) {
            const auto property =
                std::ranges::find(stage.properties, QueryPropertyId::Create(NavigationMaximumScoredCost).Value(), &QueryPropertyValue::id);
            using Cost = Result<std::optional<double>>;
            if (property == stage.properties.end())
                return Cost::Success(std::nullopt);
            const auto *cost = std::get_if<double>(&property->value);
            if (!cost || !std::isfinite(*cost) || *cost <= 0)
                return Cost::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            return Cost::Success(*cost);
        }

        std::vector<QueryPropertyValue> NavigationProperties(std::optional<double> cost, std::uint32_t weight) {
            if (!cost)
                return {};
            std::vector<QueryPropertyValue> values{{QueryPropertyId::Create(NavigationMaximumScoredCost).Value(), *cost}};
            if (weight != 1)
                values.emplace_back(QueryPropertyId::Create(NavigationScoreWeight).Value(), std::uint64_t{weight});
            return values;
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

        std::optional<QueryDistanceScoreResponse> ReadDistanceResponse(const QueryPlanStage &stage) {
            QueryDistanceScoreResponse response;
            const auto curve =
                UnsignedProperty(stage, QueryPropertyId::Create(DistanceScoreCurve).Value(), static_cast<std::uint32_t>(response.curve));
            const auto direction = UnsignedProperty(stage, QueryPropertyId::Create(DistanceScoreDirection).Value(),
                                                    static_cast<std::uint32_t>(response.direction));
            if (!curve || *curve >= static_cast<std::uint32_t>(QueryScoreCurve::Count) || !direction ||
                *direction >= static_cast<std::uint32_t>(QueryScoreDirection::Count))
                return std::nullopt;
            response.curve = static_cast<QueryScoreCurve>(*curve);
            response.direction = static_cast<QueryScoreDirection>(*direction);
            const auto threshold =
                std::ranges::find(stage.properties, QueryPropertyId::Create(DistanceScoreThreshold).Value(), &QueryPropertyValue::id);
            if (threshold != stage.properties.end()) {
                const auto *value = std::get_if<double>(&threshold->value);
                if (!value)
                    return std::nullopt;
                response.threshold = *value;
            }
            return response;
        }

        Result<QueryPointNavigationStageSettings> ReadNavigationSettings(const QueryPlanStage &stage) {
            using Settings = Result<QueryPointNavigationStageSettings>;
            const auto cost = ReadNavigationCost(stage);
            const auto weight = UnsignedProperty(stage, QueryPropertyId::Create(NavigationScoreWeight).Value(), 1);
            if (cost.HasError() || weight.value_or(0) == 0 || (!cost.Value() && *weight != 1))
                return Settings::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            return Settings::Success({stage.id, cost.Value(), *weight});
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

        Generation PublishAuthoredPoints(const AuthoredPoints &points, const QueryGeneratorOutput &output) {
            return GenerateQueryPoints(std::span{points.values}.first(points.count), output);
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
            PointOwnerSource(const Runtime::RuntimeSceneView &scene, BuiltinPointQueryCandidates candidates,
                             std::vector<PointOwnerTest> tests, QueryOwnerWorkLimits limits)
                : IOwnerQueryExecutionSource(limits), scene_(scene), candidates_(std::move(candidates)), tests_(std::move(tests)) {}

            [[nodiscard]] bool IsCurrent(const QueryExecutionInputs &inputs) const noexcept override {
                return inputs.contexts->MatchesScene(scene_) && candidates_.IsCurrent(scene_);
            }

        private:
            Result<QueryGenerationResult> GenerateAtOwner(const QueryExecutionInputs &inputs, const QueryGeneratorOutput &output,
                                                          AiWorkBudget &budget) noexcept override {
                if (!budget.Consume(WorkLimits().generationUnits))
                    return OwnerFailure<QueryGenerationResult>(AIErrors::SchedulerBudgetExhausted);
                try {
                    return candidates_.Generate(scene_, inputs, output);
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
            BuiltinPointQueryCandidates candidates_;
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

        struct PointOwnerCandidates final {
            BuiltinPointQueryCandidates values;
            QueryGeneratorProvenance provenance;
        };

        Result<PointOwnerCandidates> CapturePointCandidates(const QueryPlanStage &stage, const QueryItemTypeId itemType,
                                                            const QueryContextSnapshot &contexts, const QuerySchemaRegistry &registry,
                                                            const LivePointQueryGenerator *live = nullptr) {
            using Capture = Result<PointOwnerCandidates>;
            if (live) {
                const auto &provenance = live->provenance;
                if (stage.generator.Value() != NavigationSurfaceGenerator || !live->source || provenance.stage != stage.id ||
                    provenance.generator != stage.generator || provenance.provider != BuiltinOrigin().provider)
                    return Capture::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
                return Capture::Success({{*live}, provenance});
            }
            if (stage.generator.Value() != AuthoredPointGenerator) {
                const auto lattice = CapturePointOwnerGenerator(stage, itemType, contexts, registry);
                if (lattice.HasError())
                    return Capture::Failure(lattice.ErrorValue());
                const auto &generator = lattice.Value();
                return Capture::Success(
                    {{BuiltinDetachedPointCandidates{BuiltinDetachedPointCandidates::Lattice{generator.lattice, generator.center}}},
                     generator.provenance});
            }
            const auto *descriptor = registry.Find(stage.generator);
            const auto *item = registry.Find(itemType);
            if (!descriptor || descriptor->origin != BuiltinOrigin() || descriptor->outputItemType != itemType || !item ||
                itemType.Value() != PointItem || item->kind != QueryItemKind::Point)
                return Capture::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            auto points = CaptureAuthoredPoints(stage);
            if (points.HasError())
                return Capture::Failure(points.ErrorValue());
            return Capture::Success({{BuiltinDetachedPointCandidates{std::move(points).Value()}},
                                     {stage.id, descriptor->id, descriptor->origin.provider, contexts.Revision()}});
        }

        class GameplayPointTestSource final : public IOwnerQueryExecutionSource {
        public:
            GameplayPointTestSource(const Runtime::RuntimeSceneView &scene, BuiltinPointQueryCandidates candidates,
                                    std::shared_ptr<const GameplayQueryTestRegistry> registry, const GameplayQueryPointBinding &binding,
                                    const QueryPlanStage &stage, QueryOwnerWorkLimits limits)
                : IOwnerQueryExecutionSource(limits), scene_(scene), candidates_(std::move(candidates)), registry_(std::move(registry)),
                  binding_(&binding), stage_(stage.id), properties_(stage.properties) {}

            bool IsCurrent(const QueryExecutionInputs &inputs) const noexcept override {
                return inputs.contexts->MatchesScene(scene_) && registry_->IsCurrent() && candidates_.IsCurrent(scene_);
            }

        private:
            Result<QueryGenerationResult> GenerateAtOwner(const QueryExecutionInputs &inputs, const QueryGeneratorOutput &output,
                                                          AiWorkBudget &budget) noexcept override {
                if (!budget.Consume(WorkLimits().generationUnits))
                    return OwnerFailure<QueryGenerationResult>(AIErrors::SchedulerBudgetExhausted);
                try {
                    return candidates_.Generate(scene_, inputs, output);
                } catch (...) {
                    return OwnerFailure<QueryGenerationResult>(AIErrors::EnvironmentQueryProviderFailed);
                }
            }

            Result<QueryTestSample> TestAtOwner(const QueryExecutionInputs &inputs, QueryStageId stage, const QueryGeneratedItem &item,
                                                AiWorkBudget &budget) noexcept override {
                const auto *point = std::get_if<Math::WorldCoordinate64>(&item);
                if (stage != stage_ || !point)
                    return OwnerFailure<QueryTestSample>(AIErrors::EnvironmentQuerySchemaInvalid);
                if (!IsCurrent(inputs) || !registry_->CanEvaluate())
                    return OwnerFailure<QueryTestSample>(AIErrors::EnvironmentQueryContextStale);
                return Result<QueryTestSample>::Success(
                    binding_->evaluate(binding_->state, {*inputs.contexts, stage, *point, properties_}, budget));
            }

            Runtime::RuntimeSceneView scene_;
            BuiltinPointQueryCandidates candidates_;
            std::shared_ptr<const GameplayQueryTestRegistry> registry_;
            const GameplayQueryPointBinding *binding_;
            QueryStageId stage_;
            std::vector<QueryPropertyValue> properties_;
        };

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
        return CaptureBuiltinQuerySchema(locationContexts, {});
    }

    Result<QuerySchemaRegistry> CaptureBuiltinQuerySchema(std::span<const QueryContextDescriptor> locationContexts,
                                                          std::span<const QueryTestDescriptor> gameplayTests) {
        using enum QueryPropertyKind;
        if (gameplayTests.size() > EnvironmentQuerySchemaLimits::Tests)
            return Result<QuerySchemaRegistry>::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
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
                                    DescribeAuthoredPointGenerator(origin, item),
                                    QueryGeneratorDescriptor{
                                        .id = QueryGeneratorId::Create(NavigationSurfaceGenerator).Value(),
                                        .origin = origin,
                                        .outputItemType = item,
                                        .outputItemVersion = LatticeVersion,
                                        .contexts = {{BuiltinQueryContextId(NativePointCenters.front()), LatticeVersion}},
                                        .properties = {{QueryPropertyId::Create(NavigationSurfaceRadius).Value(), Unsigned64, true},
                                                       {QueryPropertyId::Create(NavigationSurfaceMaximum).Value(), Unsigned64, true}},
                                    },
                                    std::move(perception.generator)};
        const std::array tests{QueryTestDescriptor{
                                   .id = QueryTestId::Create(PointDistanceTest).Value(),
                                   .origin = origin,
                                   .inputItemType = item,
                                   .inputItemVersion = LatticeVersion,
                                   .properties = {{QueryPropertyId::Create(DistanceRadius).Value(), Unsigned64, true},
                                                  {QueryPropertyId::Create(DistancePreferNearer).Value(), Boolean, true},
                                                  {QueryPropertyId::Create(DistanceScoreWeight).Value(), Unsigned64, false},
                                                  {QueryPropertyId::Create(PointCenter).Value(), Unsigned64, false},
                                                  {QueryPropertyId::Create(DistanceScoreCurve).Value(), Unsigned64, false},
                                                  {QueryPropertyId::Create(DistanceScoreDirection).Value(), Unsigned64, false},
                                                  {QueryPropertyId::Create(DistanceScoreThreshold).Value(), Float64, false}},
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
                               QueryTestDescriptor{
                                   .id = QueryTestId::Create(PointNavigationTest).Value(),
                                   .origin = origin,
                                   .inputItemType = item,
                                   .inputItemVersion = LatticeVersion,
                                   .contexts = {{BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation), LatticeVersion}},
                                   .properties = {{QueryPropertyId::Create(NavigationMaximumScoredCost).Value(), Float64, false},
                                                  {QueryPropertyId::Create(NavigationScoreWeight).Value(), Unsigned64, false}},
                               },
                               std::move(perception.test)};
        if (gameplayTests.empty())
            return QuerySchemaRegistry::Capture({items, contexts, generators, tests});
        std::vector<QueryTestDescriptor> allTests(tests.begin(), tests.end());
        allTests.insert(allTests.end(), gameplayTests.begin(), gameplayTests.end());
        return QuerySchemaRegistry::Capture({items, contexts, generators, allTests});
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
        return CapturePointQueryAsset(query, result, std::move(displayName), std::move(generator), maximumResults);
    }

    bool UsesBuiltinNavigationSurface(const EnvironmentQueryPlan &plan) noexcept {
        const auto stages = plan.Stages();
        return !stages.empty() && stages.front().generator.Value() == NavigationSurfaceGenerator;
    }

    Result<EnvironmentQueryAsset> CreateBuiltinNavigationSurfaceQuery(QueryId query, QueryStageId stage, QueryResultSchemaId result,
                                                                      std::string displayName,
                                                                      const QueryNavigationSurfaceSettings settings,
                                                                      const std::uint32_t maximumResults) {
        if (settings.radiusMillimeters == 0 || settings.maximumCandidates == 0 ||
            settings.maximumCandidates > EnvironmentQuerySchemaLimits::MaximumResults)
            return Result<EnvironmentQueryAsset>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        QueryAssetStage generator{.id = stage,
                                  .kind = QueryStageKind::Generator,
                                  .generator = QueryGeneratorId::Create(NavigationSurfaceGenerator).Value(),
                                  .descriptorVersion = LatticeVersion,
                                  .properties = {{QueryPropertyId::Create(NavigationSurfaceRadius).Value(),
                                                  std::uint64_t{settings.radiusMillimeters}},
                                                 {QueryPropertyId::Create(NavigationSurfaceMaximum).Value(),
                                                  std::uint64_t{settings.maximumCandidates}}}};
        return CapturePointQueryAsset(query, result, std::move(displayName), std::move(generator), maximumResults);
    }

    /** @copydoc CreateBuiltinAuthoredPointQuery */
    Result<EnvironmentQueryAsset> CreateBuiltinAuthoredPointQuery(QueryId query, QueryStageId stage, QueryResultSchemaId result,
                                                                  std::string displayName,
                                                                  const std::span<const Math::WorldCoordinate64> points,
                                                                  const std::uint32_t maximumResults) {
        using Asset = Result<EnvironmentQueryAsset>;
        if (points.empty())
            return Asset::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        if (points.size() > MaximumBuiltinAuthoredQueryPoints)
            return Asset::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        QueryAssetStage generator{.id = stage,
                                  .kind = QueryStageKind::Generator,
                                  .generator = QueryGeneratorId::Create(AuthoredPointGenerator).Value(),
                                  .descriptorVersion = LatticeVersion};
        generator.properties.reserve(points.size());
        for (const auto &point : points)
            generator.properties.emplace_back(AuthoredPointProperty(generator.properties.size()), EncodeAuthoredPoint(point));
        return CapturePointQueryAsset(query, result, std::move(displayName), std::move(generator), maximumResults);
    }

    /** @copydoc AddBuiltinPointDistanceTest */
    Result<EnvironmentQueryAsset> AddBuiltinPointDistanceTest(const EnvironmentQueryAsset &asset, QueryStageId stage,
                                                              QueryPointDistanceTest settings) {
        using Edited = Result<EnvironmentQueryAsset>;
        if (!stage.IsValid() || !ValidDistanceSettings(settings) || !HasBuiltinPointStages(asset))
            return Edited::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        const auto stages = asset.Stages();
        const auto last = std::ranges::max_element(stages, {}, &QueryAssetStage::executionOrder);
        if (stages.size() == EnvironmentQuerySchemaLimits::Stages || last->executionOrder == std::numeric_limits<std::uint32_t>::max())
            return Edited::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        QueryAssetSource edited{.id = asset.Id(),
                                .displayName = asset.DisplayName(),
                                .result = asset.ResultSchema(),
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
        if (std::ranges::all_of(channel, [](auto byte) {
            return byte == 0;
        }))
            return Asset::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        return AppendPointProviderTest(lattice, {.id = stage,
                                                 .kind = QueryStageKind::Test,
                                                 .test = QueryTestId::Create(PointVisibilityTest).Value(),
                                                 .descriptorVersion = LatticeVersion,
                                                 .properties = VisibilityProperties(channel)});
    }

    /** @copydoc UsesBuiltinPointVisibility */
    bool UsesBuiltinPointVisibility(const EnvironmentQueryPlan &plan) noexcept {
        return std::ranges::any_of(plan.Stages(), [](const QueryPlanStage &stage) {
            return stage.test == QueryTestId::Create(PointVisibilityTest).Value();
        });
    }

    std::size_t BuiltinDetachedPointCandidates::WorkUnits() const noexcept {
        if (const auto *lattice = std::get_if<Lattice>(&source)) {
            if (!ValidLattice(lattice->policy))
                return 0;
            const std::size_t width = 2U * lattice->policy.outerSteps + 1U;
            return 2 * width * width;
        }
        const auto &points = std::get<Authored>(source);
        return points.count <= points.values.size() ? points.count : 0;
    }

    Result<QueryGenerationResult> BuiltinDetachedPointCandidates::Generate(const QueryGeneratorOutput &output) const {
        if (!output.IsValid() || WorkUnits() == 0)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        if (const auto *lattice = std::get_if<Lattice>(&source))
            return GenerateLatticeAtPoint(lattice->center, lattice->policy, output);
        return PublishAuthoredPoints(std::get<Authored>(source), output);
    }

    bool BuiltinPointQueryCandidates::IsCurrent(const Runtime::RuntimeSceneView &scene) const noexcept {
        const auto *live = std::get_if<LivePointQueryGenerator>(&source);
        return !live || (live->source && live->source->IsCurrent(scene));
    }

    std::size_t BuiltinPointQueryCandidates::WorkUnits() const noexcept {
        if (const auto *live = std::get_if<LivePointQueryGenerator>(&source))
            return live->workUnits;
        return std::get<BuiltinDetachedPointCandidates>(source).WorkUnits();
    }

    Result<QueryGenerationResult> BuiltinPointQueryCandidates::Generate(const Runtime::RuntimeSceneView &scene,
                                                                        const QueryExecutionInputs &inputs,
                                                                        const QueryGeneratorOutput &output) const {
        if (const auto *live = std::get_if<LivePointQueryGenerator>(&source)) {
            if (!live->source)
                return Generation::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            return live->source->Generate(scene, inputs, output);
        }
        return std::get<BuiltinDetachedPointCandidates>(source).Generate(output);
    }

    /** @copydoc CaptureBuiltinPointVisibility */
    Result<BuiltinPointVisibilityPlan> CaptureBuiltinPointVisibility(const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry,
                                                                     const QueryContextSnapshot &contexts,
                                                                     const LivePointQueryGenerator *live) {
        using Captured = Result<BuiltinPointVisibilityPlan>;
        const auto stages = plan.Stages();
        const auto *visibility = FindPointProviderTest(plan, PointVisibilityTest);
        if (!visibility || contexts.Query() != plan.Id())
            return Captured::Failure(MakeError(AIErrors::EnvironmentQueryStageUnsupported));

        const auto *test = registry.Find(visibility->test);
        if (!test || test->origin != BuiltinOrigin())
            return Captured::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        auto generator = CapturePointCandidates(stages.front(), plan.ResultSchema().itemType, contexts, registry, live);
        if (generator.HasError())
            return Captured::Failure(generator.ErrorValue());
        const auto channel = ReadVisibilityChannel(*visibility);
        if (!channel)
            return Captured::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        return Captured::Success({std::move(generator.Value().values), generator.Value().provenance, visibility->id, *channel});
    }

    /** @copydoc AddBuiltinPointNavigationTest */
    Result<EnvironmentQueryAsset> AddBuiltinPointNavigationTest(const EnvironmentQueryAsset &lattice, const QueryStageId stage,
                                                                const std::optional<double> maximumScoredCost,
                                                                const std::uint32_t scoreWeight) {
        using Asset = Result<EnvironmentQueryAsset>;
        if (scoreWeight == 0 || (!maximumScoredCost && scoreWeight != 1) ||
            (maximumScoredCost && (!std::isfinite(*maximumScoredCost) || *maximumScoredCost <= 0)))
            return Asset::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        QueryAssetStage path{.id = stage,
                             .kind = QueryStageKind::Test,
                             .test = QueryTestId::Create(PointNavigationTest).Value(),
                             .descriptorVersion = LatticeVersion,
                             .properties = NavigationProperties(maximumScoredCost, scoreWeight)};
        return AppendPointProviderTest(lattice, std::move(path));
    }

    /** @copydoc UsesBuiltinPointNavigation */
    bool UsesBuiltinPointNavigation(const EnvironmentQueryPlan &plan) noexcept {
        return std::ranges::any_of(plan.Stages(), [](const QueryPlanStage &stage) {
            return stage.test == QueryTestId::Create(PointNavigationTest).Value();
        });
    }

    /** @copydoc BuiltinPointNavigationPlan::Rule */
    QueryScoreRule BuiltinPointNavigationPlan::Rule() const {
        QueryScoreRule rule{.stage = test, .requiredFilterValue = true};
        if (maximumScoredCost)
            rule.score = QueryScoreRange{.maximum = *maximumScoredCost,
                                         .weight = static_cast<double>(scoreWeight),
                                         .direction = QueryScoreDirection::Lower};
        return rule;
    }

    /** @copydoc CaptureBuiltinPointNavigation */
    Result<BuiltinPointNavigationPlan> CaptureBuiltinPointNavigation(const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry,
                                                                     const QueryContextSnapshot &contexts,
                                                                     const LivePointQueryGenerator *live) {
        using Captured = Result<BuiltinPointNavigationPlan>;
        const auto stages = plan.Stages();
        const auto *navigation = FindPointProviderTest(plan, PointNavigationTest);
        if (!navigation || contexts.Query() != plan.Id())
            return Captured::Failure(MakeError(AIErrors::EnvironmentQueryStageUnsupported));
        const auto &path = *navigation;
        const auto *descriptor = registry.Find(path.test);
        if (!descriptor || descriptor->origin != BuiltinOrigin())
            return Captured::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        auto generator = CapturePointCandidates(stages.front(), plan.ResultSchema().itemType, contexts, registry, live);
        if (generator.HasError())
            return Captured::Failure(generator.ErrorValue());
        const auto settings = ReadNavigationSettings(path);
        if (settings.HasError())
            return Captured::Failure(settings.ErrorValue());
        return Captured::Success({std::move(generator.Value().values), generator.Value().provenance, path.id,
                                  settings.Value().maximumScoredCost, settings.Value().scoreWeight});
    }

    /** @copydoc RemoveBuiltinPointDistanceTest */
    Result<EnvironmentQueryAsset> RemoveBuiltinPointDistanceTest(const EnvironmentQueryAsset &lattice, QueryStageId stage) {
        const auto test = std::ranges::find(lattice.Stages(), stage, &QueryAssetStage::id);
        if (test == lattice.Stages().end() || test->test.Value() != PointDistanceTest)
            return Result<EnvironmentQueryAsset>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        return RemoveBuiltinPointTest(lattice, stage);
    }

    /** @copydoc RemoveBuiltinPointTest */
    Result<EnvironmentQueryAsset> RemoveBuiltinPointTest(const EnvironmentQueryAsset &lattice, QueryStageId stage) {
        const auto stages = lattice.Stages();
        const auto test = std::ranges::find(stages, stage, &QueryAssetStage::id);
        const std::array known{PointDistanceTest, PointVisibilityTest, PointNavigationTest};
        if (test == stages.end() || test->kind != QueryStageKind::Test || std::ranges::find(known, test->test.Value()) == known.end())
            return Result<EnvironmentQueryAsset>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        QueryAssetSource edited{.id = lattice.Id(),
                                .displayName = lattice.DisplayName(),
                                .result = lattice.ResultSchema(),
                                .stages = {stages.begin(), stages.end()}};
        edited.stages.erase(edited.stages.begin() + (test - stages.begin()));
        return EnvironmentQueryAsset::Capture(edited);
    }

    namespace {
        Result<bool> ReadPointProviderSettings(const QueryPlanStage &stage, const QuerySchemaRegistry &registry,
                                               BuiltinPointQuerySettings &settings) {
            using Read = Result<bool>;
            const auto provider = stage.test.Value();
            if (provider != PointVisibilityTest && provider != PointNavigationTest)
                return Read::Success(false);
            const auto *descriptor = registry.Find(stage.test);
            if (!descriptor || descriptor->origin != BuiltinOrigin())
                return Read::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            if (provider == PointVisibilityTest) {
                const auto channel = ReadVisibilityChannel(stage);
                if (!channel || settings.visibility)
                    return Read::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
                settings.visibility.emplace(stage.id, *channel);
                return Read::Success(true);
            }
            const auto navigation = ReadNavigationSettings(stage);
            if (navigation.HasError() || settings.navigation)
                return Read::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            settings.navigation = navigation.Value();
            return Read::Success(true);
        }

        Result<void> ReadPointGeneratorSettings(const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry,
                                                BuiltinPointQuerySettings &settings) {
            using Read = Result<void>;
            const auto &stage = plan.Stages().front();
            const auto *descriptor = registry.Find(stage.generator);
            if (!descriptor || descriptor->origin != BuiltinOrigin() || descriptor->outputItemType.Value() != PointItem)
                return Read::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            if (UsesBuiltinNavigationSurface(plan)) {
                const auto surface = ReadBuiltinNavigationSurface(plan, registry);
                if (surface.HasError())
                    return Read::Failure(surface.ErrorValue());
                settings.alternateGenerator = surface.Value();
                return Read::Success();
            }
            if (stage.generator.Value() == AuthoredPointGenerator) {
                const auto points = CaptureAuthoredPoints(stage);
                if (points.HasError())
                    return Read::Failure(points.ErrorValue());
                const auto first = points.Value().values.begin();
                settings.alternateGenerator = std::vector<Math::WorldCoordinate64>(first, first + points.Value().count);
                return Read::Success();
            }
            if (stage.generator.Value() != LatticeGenerator)
                return Read::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            const auto lattice = PlanLattice(stage);
            if (!lattice || !ValidLattice(*lattice))
                return Read::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            settings.lattice = *lattice;
            settings.latticeCenters = PointCenterOptions(descriptor->contextChoice, registry);
            return Read::Success();
        }

        Result<EnvironmentQueryAsset> CreateEditedPointGenerator(const EnvironmentQueryAsset &asset, const QueryStageId stage,
                                                                 const BuiltinPointQuerySettings &settings) {
            const auto id = asset.ResultSchema().id;
            if (const auto *surface = std::get_if<QueryNavigationSurfaceSettings>(&settings.alternateGenerator))
                return CreateBuiltinNavigationSurfaceQuery(asset.Id(), stage, id, asset.DisplayName(), *surface, settings.maximumResults);
            if (const auto *points = std::get_if<std::vector<Math::WorldCoordinate64>>(&settings.alternateGenerator))
                return CreateBuiltinAuthoredPointQuery(asset.Id(), stage, id, asset.DisplayName(), *points, settings.maximumResults);
            return CreateBuiltinLatticeQuery(asset.Id(), stage, id, asset.DisplayName(), settings.lattice, settings.maximumResults);
        }

        void WritePointTestSettings(QueryAssetStage &stage, const BuiltinPointQuerySettings &settings) {
            if (stage.test.Value() == PointVisibilityTest) {
                stage.properties = VisibilityProperties(settings.visibility->channel);
                return;
            }
            if (stage.test.Value() == PointNavigationTest) {
                stage.properties = NavigationProperties(settings.navigation->maximumScoredCost, settings.navigation->scoreWeight);
                return;
            }
            if (stage.test.Value() == PointDistanceTest) {
                const auto entry = std::ranges::find(settings.distanceTests, stage.id, &QueryPointDistanceStageSettings::stage);
                stage.properties = DistanceProperties(entry->test);
            }
        }
    }  // namespace

    /** @copydoc ReadBuiltinPointQuerySettings */
    Result<BuiltinPointQuerySettings> ReadBuiltinPointQuerySettings(const EnvironmentQueryAsset &asset,
                                                                    const QuerySchemaRegistry &registry) {
        using Settings = Result<BuiltinPointQuerySettings>;
        const auto plan = EnvironmentQueryPlan::Compile(asset, registry);
        if (plan.HasError())
            return Settings::Failure(plan.ErrorValue());
        const auto stages = plan.Value().Stages();
        BuiltinPointQuerySettings settings{.maximumResults = asset.ResultSchema().maximumResults};
        const auto generator = ReadPointGeneratorSettings(plan.Value(), registry, settings);
        if (generator.HasError())
            return Settings::Failure(generator.ErrorValue());
        settings.distanceTests.reserve(stages.size() - 1);
        for (const auto &stage : stages.subspan(1)) {
            const auto provider = ReadPointProviderSettings(stage, registry, settings);
            if (provider.HasError())
                return Settings::Failure(provider.ErrorValue());
            const auto *descriptor = registry.Find(stage.test);
            if (provider.Value() || (descriptor && IsGameplayPointTest(*descriptor)))
                continue;
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
        const auto identity = [](const auto &stage) {
            return stage ? std::optional{stage->stage} : std::nullopt;
        };
        if (previous.Value().alternateGenerator.index() != settings.alternateGenerator.index() ||
            !std::ranges::equal(previous.Value().distanceTests, settings.distanceTests, {}, stageId, stageId) ||
            identity(previous.Value().visibility) != identity(settings.visibility) ||
            identity(previous.Value().navigation) != identity(settings.navigation) ||
            (settings.navigation &&
             (settings.navigation->scoreWeight == 0 || (!settings.navigation->maximumScoredCost && settings.navigation->scoreWeight != 1))))
            return Edited::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));

        const auto generator = std::ranges::find(asset.Stages(), QueryStageKind::Generator, &QueryAssetStage::kind);
        auto generated = CreateEditedPointGenerator(asset, generator->id, settings);
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
            WritePointTestSettings(stage, settings);
        }
        auto captured = EnvironmentQueryAsset::Capture(edited);
        if (captured.HasError())
            return captured;
        const auto verified = ReadBuiltinPointQuerySettings(captured.Value(), registry);
        return verified.HasError() ? Edited::Failure(verified.ErrorValue()) : std::move(captured);
    }

    bool QueryDistanceScoreResponse::IsValid() const noexcept {
        return curve < QueryScoreCurve::Count && direction < QueryScoreDirection::Count && std::isfinite(threshold) && threshold >= 0 &&
               threshold <= 1 && (curve == QueryScoreCurve::Threshold || threshold == QueryDistanceScoreResponse{}.threshold);
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
        const auto response = ReadDistanceResponse(stage);
        if (!radius || !weight || !scoring || !response)
            return Captured::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        const QueryPointDistanceTest settings{*radius, *scoring, *weight, stage.selectedContext, *response};
        if (!ValidDistanceSettings(settings))
            return Captured::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        return Captured::Success(BuiltinPointDistanceQuery{stage.id, settings});
    }

    /** @copydoc BuiltinPointDistanceQuery::Rule */
    QueryScoreRule BuiltinPointDistanceQuery::Rule() const {
        QueryScoreRule rule{.stage = stage_, .requiredFilterValue = true};
        if (settings_.preferNearer)
            rule.score = QueryScoreRange{.maximum = settings_.radiusMillimeters / 1000.0,
                                         .weight = static_cast<double>(settings_.scoreWeight),
                                         .curve = settings_.response.curve,
                                         .direction = settings_.response.direction,
                                         .threshold = settings_.response.threshold};
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
        if (stage == plan.Stages().end() || stage->kind != QueryStageKind::Generator || stage->generator != output.provenance.generator ||
            output.provenance.provider.Value() != LatticeProvider ||
            plan.ResultSchema().itemType != QueryItemTypeId::Create(PointItem).Value())
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        if (contexts.Query() != plan.Id())
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        if (stage->generator.Value() == AuthoredPointGenerator) {
            if (!output.IsValid())
                return Generation::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            if (contexts.Revision() != output.provenance.revision)
                return Generation::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
            const auto points = CaptureAuthoredPoints(*stage);
            return points.HasError() ? Generation::Failure(points.ErrorValue()) : PublishAuthoredPoints(points.Value(), output);
        }
        if (stage->generator.Value() != LatticeGenerator)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        const auto lattice = PlanLattice(*stage);
        if (!lattice)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        return GenerateQueryLattice(contexts, lattice->center, *lattice, output);
    }

    QueryItemTypeId BuiltinQueryPointItemType() noexcept {
        return QueryItemTypeId::Create(PointItem).Value();
    }

    bool IsGameplayPointTest(const QueryTestDescriptor &descriptor) noexcept {
        return descriptor.inputItemType.Value() == PointItem && descriptor.origin.kind == QueryDescriptorSourceKind::Native &&
               descriptor.origin != BuiltinOrigin();
    }

    namespace {
        Result<void> UpdateGameplayPointStage(QueryAssetSource &source, const QueryTestDescriptor &descriptor, const QueryStageId stage,
                                              const std::optional<std::span<const QueryPropertyValue>> properties) {
            using Changed = Result<void>;
            const auto existing = std::ranges::find(source.stages, stage, &QueryAssetStage::id);
            if (existing != source.stages.end()) {
                if (existing->kind != QueryStageKind::Test || existing->test != descriptor.id)
                    return Changed::Failure(MakeError(AIErrors::EnvironmentQueryIdentityConflict));
                if (properties)
                    existing->properties.assign(properties->begin(), properties->end());
                else
                    source.stages.erase(existing);
                return Changed::Success();
            }
            if (!properties)
                return Changed::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            const auto last = std::ranges::max_element(source.stages, {}, &QueryAssetStage::executionOrder);
            if (source.stages.size() >= EnvironmentQuerySchemaLimits::Stages || last == source.stages.end() ||
                last->executionOrder == std::numeric_limits<std::uint32_t>::max())
                return Changed::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
            source.stages.push_back({.id = stage,
                                     .kind = QueryStageKind::Test,
                                     .executionOrder = last->executionOrder + 1,
                                     .test = descriptor.id,
                                     .descriptorVersion = {descriptor.origin.version, descriptor.origin.version},
                                     .properties = {properties->begin(), properties->end()}});
            return Changed::Success();
        }
    }  // namespace

    Result<EnvironmentQueryAsset> EditGameplayPointTest(const EnvironmentQueryAsset &source, const QuerySchemaRegistry &registry,
                                                        const QueryTestId test, const QueryStageId stage,
                                                        const std::optional<std::span<const QueryPropertyValue>> properties) {
        using Edited = Result<EnvironmentQueryAsset>;
        const auto *descriptor = registry.Find(test);
        if (!descriptor || !IsGameplayPointTest(*descriptor) || !HasBuiltinPointStages(source) || !stage.IsValid())
            return Edited::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        if (properties && properties->size() > EnvironmentQuerySchemaLimits::PropertiesPerStage)
            return Edited::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        QueryAssetSource edited{.id = source.Id(),
                                .displayName = source.DisplayName(),
                                .result = source.ResultSchema(),
                                .stages = {source.Stages().begin(), source.Stages().end()}};
        if (const auto changed = UpdateGameplayPointStage(edited, *descriptor, stage, properties); changed.HasError())
            return Edited::Failure(changed.ErrorValue());
        auto captured = EnvironmentQueryAsset::Capture(edited);
        if (captured.HasError())
            return captured;
        const auto validated = EnvironmentQueryPlan::Compile(captured.Value(), registry);
        return validated.HasError() ? Edited::Failure(validated.ErrorValue()) : std::move(captured);
    }

    Result<GameplayQueryTestRegistry> GameplayQueryTestRegistry::Capture(const QuerySchemaRegistry &schema,
                                                                         const std::span<const GameplayQueryPointBinding> bindings,
                                                                         std::shared_ptr<const void> generation,
                                                                         CancellationToken retirement, NativeGameplayQueryAdmission native,
                                                                         const std::span<const QueryTestId> deterministic) {
        using Registry = Result<GameplayQueryTestRegistry>;
        if (bindings.size() > EnvironmentQuerySchemaLimits::Tests || deterministic.size() > bindings.size())
            return Registry::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        if (!generation || retirement.IsCancellationRequested())
            return Registry::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        GameplayQueryTestRegistry captured;
        captured.generation_ = std::move(generation);
        captured.retirement_ = std::move(retirement);
        captured.native_ = std::move(native);
        captured.bindings_.reserve(bindings.size());
        for (const auto &binding : bindings) {
            const auto *descriptor = schema.Find(binding.test);
            if (!descriptor || descriptor->origin == BuiltinOrigin() || descriptor->origin != binding.origin ||
                descriptor->inputItemType.Value() != PointItem || !binding.evaluate)
                return Registry::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            if ((!binding.requiredFilter && !binding.score) || binding.workUnits == 0 ||
                binding.workUnits > EnvironmentQuerySchemaLimits::MaximumResults * 16U)
                return Registry::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            captured.bindings_.push_back(binding);
        }
        std::ranges::sort(captured.bindings_, {}, &GameplayQueryPointBinding::test);
        if (std::ranges::adjacent_find(captured.bindings_, {}, &GameplayQueryPointBinding::test) != captured.bindings_.end())
            return Registry::Failure(MakeError(AIErrors::EnvironmentQueryIdentityConflict));
        captured.deterministic_.assign(deterministic.begin(), deterministic.end());
        std::ranges::sort(captured.deterministic_);
        if (std::ranges::adjacent_find(captured.deterministic_) != captured.deterministic_.end())
            return Registry::Failure(MakeError(AIErrors::EnvironmentQueryIdentityConflict));
        for (const auto test : captured.deterministic_) {
            const auto *binding = captured.Find(test);
            if (!binding || binding->state)
                return Registry::Failure(MakeError(AIErrors::EnvironmentQueryStageUnsupported));
        }
        return Registry::Success(std::move(captured));
    }

    bool GameplayQueryTestRegistry::IsCurrent() const noexcept {
        return generation_ && !retirement_.IsCancellationRequested() &&
               (!native_.current || native_.current->load(std::memory_order_acquire));
    }

    bool GameplayQueryTestRegistry::CanEvaluate() const noexcept {
        return IsCurrent() && (!native_.active || native_.active->load(std::memory_order_acquire));
    }

    const GameplayQueryPointBinding *GameplayQueryTestRegistry::Find(const QueryTestId test) const noexcept {
        const auto found = std::ranges::lower_bound(bindings_, test, {}, &GameplayQueryPointBinding::test);
        return found != bindings_.end() && found->test == test ? &*found : nullptr;
    }

    bool GameplayQueryTestRegistry::Contains(const QueryTestId test) const noexcept {
        return Find(test) != nullptr;
    }

    bool GameplayQueryTestRegistry::UsesTests(const EnvironmentQueryPlan &plan) const noexcept {
        return std::ranges::any_of(plan.Stages(), [this](const auto &stage) {
            return Find(stage.test) != nullptr;
        });
    }

    bool GameplayQueryTestRegistry::SupportsDeterministic(const EnvironmentQueryPlan &plan) const noexcept {
        return std::ranges::all_of(plan.Stages(), [this](const auto &stage) {
            return !Contains(stage.test) || std::ranges::binary_search(deterministic_, stage.test);
        });
    }

    Result<PreparedPointQueryTest> PrepareGameplayPointTest(const EnvironmentQueryPlan &plan, const QueryStageId stageId,
                                                            const QuerySchemaRegistry &schema, const Runtime::RuntimeSceneView &scene,
                                                            const QueryExecutionInputs &inputs, const QueryExecutionSettings &settings,
                                                            const std::shared_ptr<const GameplayQueryTestRegistry> &gameplay,
                                                            const LivePointQueryGenerator *live) {
        using Prepared = Result<PreparedPointQueryTest>;
        if (!gameplay || !gameplay->IsCurrent() || !inputs.contexts || !inputs.contexts->MatchesScene(scene) ||
            inputs.contexts->Query() != plan.Id())
            return Prepared::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto stage = std::ranges::find(plan.Stages(), stageId, &QueryPlanStage::id);
        if (stage == plan.Stages().end() || stage->kind != QueryStageKind::Test)
            return Prepared::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        const auto *binding = gameplay->Find(stage->test);
        const auto *descriptor = schema.Find(stage->test);
        if (!binding || !descriptor || descriptor->origin != binding->origin)
            return Prepared::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        auto candidates = CapturePointCandidates(plan.Stages().front(), plan.ResultSchema().itemType, *inputs.contexts, schema, live);
        if (candidates.HasError())
            return Prepared::Failure(candidates.ErrorValue());
        auto captured = std::move(candidates).Value();
        const QueryOwnerWorkLimits limits{captured.values.WorkUnits(), binding->workUnits};
        const QueryScoreRule rule{stageId, binding->requiredFilter, binding->score};
        auto source = std::make_unique<GameplayPointTestSource>(scene, std::move(captured.values), gameplay, *binding, *stage, limits);
        return Prepared::Success({captured.provenance, rule, limits.generationUnits,
                                  live ? live->maximumCandidates : settings.maximumCandidates, limits.testUnits, std::move(source)});
    }

    /** @copydoc AdmitBuiltinPointProviderQuery */
    Result<std::unique_ptr<QueryExecution>> AdmitBuiltinPointProviderQuery(const EnvironmentQueryPlan &plan,
                                                                           const QuerySchemaRegistry &registry,
                                                                           const Runtime::RuntimeSceneView &scene,
                                                                           QueryExecutionInputs inputs, QueryExecutionSettings settings,
                                                                           std::span<PreparedPointQueryTest> providers,
                                                                           const LivePointQueryGenerator *live, QueryExecutionMode mode) {
        using Admission = Result<std::unique_ptr<QueryExecution>>;
        if (!inputs.contexts || !inputs.contexts->MatchesScene(scene) || inputs.contexts->Query() != plan.Id())
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto stages = plan.Stages();
        const auto distance = [](const QueryPlanStage &stage) {
            return stage.test.Value() == PointDistanceTest;
        };
        if (std::ranges::none_of(stages, distance))
            return AdmitPreparedPointQuery(plan, registry, std::move(inputs), settings, providers, mode);
        if (providers.empty() || providers.size() + std::ranges::count_if(stages, distance) >= EnvironmentQuerySchemaLimits::Stages)
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryStageUnsupported));
        const auto generator = CapturePointCandidates(stages.front(), plan.ResultSchema().itemType, *inputs.contexts, registry, live);
        if (generator.HasError())
            return Admission::Failure(generator.ErrorValue());
        std::vector<PreparedPointQueryTest> tests;
        tests.reserve(stages.size() - 1);
        for (auto &provider : providers)
            tests.push_back(std::move(provider));
        const auto &captured = generator.Value();
        const QueryOwnerWorkLimits limits{captured.values.WorkUnits(), 2};
        for (const auto &stage : stages | std::views::filter(distance)) {
            auto local = CapturePointOwnerTests(std::span{&stage, 1}, *inputs.contexts, registry);
            if (local.HasError())
                return Admission::Failure(local.ErrorValue());
            auto prepared = std::move(local).Value();
            auto source = std::make_unique<PointOwnerSource>(scene, captured.values, std::move(prepared.kernels), limits);
            tests.push_back({captured.provenance, prepared.rules.front(), limits.generationUnits, settings.maximumCandidates,
                             limits.testUnits, std::move(source)});
        }
        return AdmitPreparedPointQuery(plan, registry, std::move(inputs), settings, tests, mode);
    }

    Result<QueryNavigationSurfaceSettings> ReadBuiltinNavigationSurface(const EnvironmentQueryPlan &plan,
                                                                        const QuerySchemaRegistry &registry) {
        using Settings = Result<QueryNavigationSurfaceSettings>;
        if (!UsesBuiltinNavigationSurface(plan) || plan.ResultSchema().itemType.Value() != PointItem)
            return Settings::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        const auto &stage = plan.Stages().front();
        const auto *descriptor = registry.Find(stage.generator);
        if (!descriptor || descriptor->origin != BuiltinOrigin() || descriptor->outputItemType.Value() != PointItem)
            return Settings::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        const auto radius = UnsignedProperty(stage, QueryPropertyId::Create(NavigationSurfaceRadius).Value());
        const auto maximum = UnsignedProperty(stage, QueryPropertyId::Create(NavigationSurfaceMaximum).Value());
        if (radius.value_or(0) == 0 || maximum.value_or(0) == 0 || *maximum > EnvironmentQuerySchemaLimits::MaximumResults)
            return Settings::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        return Settings::Success({*radius, *maximum});
    }

    Result<std::unique_ptr<QueryExecution>> AdmitBoundBuiltinPointQuery(const EnvironmentQueryPlan &plan,
                                                                        const QuerySchemaRegistry &registry,
                                                                        const Runtime::RuntimeSceneView &scene, QueryExecutionInputs inputs,
                                                                        QueryExecutionSettings settings,
                                                                        const LivePointQueryGenerator &generator) {
        using Admission = Result<std::unique_ptr<QueryExecution>>;
        const auto stages = plan.Stages();
        if (!generator.source || stages.empty() || !inputs.contexts || !inputs.contexts->MatchesScene(scene))
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        if (generator.maximumCandidates > settings.maximumCandidates)
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        auto tests = CapturePointOwnerTests(stages.subspan(1), *inputs.contexts, registry);
        if (tests.HasError())
            return Admission::Failure(tests.ErrorValue());
        auto scoring = QueryScoringPlan::Capture(plan, registry, tests.Value().rules, QueryScoreTieBreak::StableIndex);
        if (scoring.HasError())
            return Admission::Failure(scoring.ErrorValue());
        settings.generatorWorkUnits = generator.workUnits;
        settings.maximumCandidates = generator.maximumCandidates;
        const QueryOwnerWorkLimits limits{settings.generatorWorkUnits, 1U + std::bit_width(tests.Value().kernels.size())};
        auto source =
            std::make_unique<PointOwnerSource>(scene, BuiltinPointQueryCandidates{generator}, std::move(tests).Value().kernels, limits);
        return QueryExecution::AdmitDeterministic(std::move(inputs), generator.provenance, std::move(scoring).Value(), settings,
                                                  std::move(source));
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
        auto generator = CapturePointCandidates(stages.front(), plan.ResultSchema().itemType, *inputs.contexts, registry);
        if (generator.HasError())
            return Admission::Failure(generator.ErrorValue());
        auto tests = CapturePointOwnerTests(stages.subspan(1), *inputs.contexts, registry);
        if (tests.HasError())
            return Admission::Failure(tests.ErrorValue());
        auto scoring = QueryScoringPlan::Capture(plan, registry, tests.Value().rules, QueryScoreTieBreak::StableIndex);
        if (scoring.HasError())
            return Admission::Failure(scoring.ErrorValue());

        auto captured = std::move(generator).Value();
        // Reserve the complete generation pass and one stage lookup plus distance evaluation.
        const QueryOwnerWorkLimits limits{captured.values.WorkUnits(), 1U + std::bit_width(tests.Value().kernels.size())};
        settings.generatorWorkUnits = limits.generationUnits;
        auto source = std::make_unique<PointOwnerSource>(scene, std::move(captured.values), std::move(tests).Value().kernels, limits);
        const auto mode = reuse.cache ? QueryExecutionMode::AdaptiveRealtime : QueryExecutionMode::DeterministicWorkUnits;
        auto admitted = QueryExecution::AdmitOwner(std::move(inputs), captured.provenance, std::move(scoring).Value(), settings,
                                                   std::move(source), mode);
        if (reuse.cache && admitted.HasValue() && HasOnlyBuiltinPointContexts(plan))
            admitted.Value()->BindPointCache(scene, reuse);
        return admitted;
    }

}  // namespace Horo::AI
