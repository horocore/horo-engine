#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQuerySchema.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Converts the active variant arm to its exact typed property contract. */
        [[nodiscard]] QueryPropertyKind PropertyKind(const QueryPropertyData &value) noexcept {
            using enum QueryPropertyKind;
            switch (value.index()) {
                case 0:
                    return Boolean;
                case 1:
                    return Signed64;
                case 2:
                    return Unsigned64;
                case 3:
                    return Float64;
                case 4:
                    return CanonicalBytes;
                default:
                    return Count;
            }
        }

        /** @brief Checks a typed authored property without interpreting unknown provider data. */
        [[nodiscard]] Result<void> ValidateValue(const QueryPropertyValue &property) {
            if (!property.id.IsValid() || PropertyKind(property.value) == QueryPropertyKind::Count)
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            if (const auto *number = std::get_if<double>(&property.value); number != nullptr && !std::isfinite(*number))
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            if (const auto *bytes = std::get_if<std::vector<std::byte>>(&property.value);
                bytes != nullptr && bytes->size() > EnvironmentQuerySchemaLimits::CanonicalBytes)
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
            return Result<void>::Success();
        }

        /** @brief Validates bounded stage representation while retaining future unknown bytes. */
        [[nodiscard]] Result<void> ValidateStage(const QueryAssetStage &stage) {
            using enum QueryStageKind;
            if (!stage.id.IsValid() || !stage.descriptorVersion.IsValid() || stage.kind >= Count)
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            if (stage.displayName.size() > EnvironmentQuerySchemaLimits::DisplayNameBytes ||
                stage.properties.size() > EnvironmentQuerySchemaLimits::PropertiesPerStage ||
                stage.opaquePayload.size() > EnvironmentQuerySchemaLimits::UnknownStageBytes)
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
            if ((stage.kind == Generator &&
                 (!stage.generator.IsValid() || stage.test.IsValid() || stage.unknownTypeId != 0 || !stage.opaquePayload.empty())) ||
                (stage.kind == Test &&
                 (!stage.test.IsValid() || stage.generator.IsValid() || stage.unknownTypeId != 0 || !stage.opaquePayload.empty())) ||
                (stage.kind == Unknown && (stage.unknownTypeId == 0 || stage.generator.IsValid() || stage.test.IsValid())))
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            for (std::size_t index = 0; index < stage.properties.size(); ++index) {
                if (const auto result = ValidateValue(stage.properties[index]); result.HasError())
                    return result;
                for (std::size_t other = index + 1; other < stage.properties.size(); ++other)
                    if (stage.properties[index].id == stage.properties[other].id)
                        return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryIdentityConflict));
            }
            return Result<void>::Success();
        }

        /** @brief Rejects unknown, duplicated, missing, or type-mismatched stage properties. */
        [[nodiscard]] Result<void> ResolveProperties(const QueryAssetStage &stage,
                                                     const std::vector<QueryPropertyDescriptor> &descriptors) {
            for (const auto &property : stage.properties) {
                const auto found = std::ranges::find_if(descriptors, [&property](const auto &entry) {
                    return entry.id == property.id;
                });
                if (found == descriptors.end())
                    return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
                if (PropertyKind(property.value) != found->kind)
                    return Result<void>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
                if (const auto *bytes = std::get_if<std::vector<std::byte>>(&property.value);
                    bytes != nullptr && bytes->size() > found->maximumBytes)
                    return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
            }
            for (const auto &descriptor : descriptors) {
                if (descriptor.required && std::ranges::none_of(stage.properties, [&descriptor](const auto &entry) {
                    return entry.id == descriptor.id;
                }))
                    return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            }
            return Result<void>::Success();
        }

        /** @brief Admits a known generator or test without ever dropping an unsupported stage. */
        [[nodiscard]] Result<QueryPlanStage> ResolveStage(const QueryAssetStage &stage, const QuerySchemaRegistry &registry,
                                                          const QueryResultSchema &result) {
            if (stage.kind == QueryStageKind::Unknown)
                return Result<QueryPlanStage>::Failure(MakeError(AIErrors::EnvironmentQueryStageUnsupported));
            const std::vector<QueryContextRequirement> *contexts{};
            if (stage.kind == QueryStageKind::Generator) {
                const auto *descriptor = registry.Find(stage.generator);
                if (descriptor == nullptr)
                    return Result<QueryPlanStage>::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
                if (!stage.descriptorVersion.Contains(descriptor->origin.version))
                    return Result<QueryPlanStage>::Failure(MakeError(AIErrors::EnvironmentQueryVersionIncompatible));
                if (descriptor->outputItemType != result.itemType)
                    return Result<QueryPlanStage>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
                if (const auto properties = ResolveProperties(stage, descriptor->properties); properties.HasError())
                    return Result<QueryPlanStage>::Failure(properties.ErrorValue());
                contexts = &descriptor->contexts;
            } else {
                const auto *descriptor = registry.Find(stage.test);
                if (descriptor == nullptr)
                    return Result<QueryPlanStage>::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
                if (!stage.descriptorVersion.Contains(descriptor->origin.version))
                    return Result<QueryPlanStage>::Failure(MakeError(AIErrors::EnvironmentQueryVersionIncompatible));
                if (descriptor->inputItemType != result.itemType)
                    return Result<QueryPlanStage>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
                if (const auto properties = ResolveProperties(stage, descriptor->properties); properties.HasError())
                    return Result<QueryPlanStage>::Failure(properties.ErrorValue());
                contexts = &descriptor->contexts;
            }
            return Result<QueryPlanStage>::Success({.id = stage.id,
                                                    .kind = stage.kind,
                                                    .executionOrder = stage.executionOrder,
                                                    .generator = stage.generator,
                                                    .test = stage.test,
                                                    .contexts = *contexts,
                                                    .properties = stage.properties});
        }
    }  // namespace

    EnvironmentQueryAsset::EnvironmentQueryAsset(QueryAssetSource source) noexcept : source_(std::move(source)) {}

    /** @copydoc EnvironmentQueryAsset::Capture */
    Result<EnvironmentQueryAsset> EnvironmentQueryAsset::Capture(const QueryAssetSource &source) {
        if (!source.id.IsValid() || !source.result.id.IsValid() || source.result.version == 0 || !source.result.itemType.IsValid() ||
            !source.result.itemVersion.IsValid())
            return Result<EnvironmentQueryAsset>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        if (source.schemaVersion != CurrentEnvironmentQuerySchemaVersion)
            return Result<EnvironmentQueryAsset>::Failure(MakeError(AIErrors::EnvironmentQueryVersionIncompatible));
        if (source.displayName.size() > EnvironmentQuerySchemaLimits::DisplayNameBytes || source.stages.empty() ||
            source.stages.size() > EnvironmentQuerySchemaLimits::Stages || source.result.maximumResults == 0 ||
            source.result.maximumResults > EnvironmentQuerySchemaLimits::MaximumResults)
            return Result<EnvironmentQueryAsset>::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        for (std::size_t index = 0; index < source.stages.size(); ++index) {
            if (const auto stage = ValidateStage(source.stages[index]); stage.HasError())
                return Result<EnvironmentQueryAsset>::Failure(stage.ErrorValue());
            for (std::size_t other = index + 1; other < source.stages.size(); ++other)
                if (source.stages[index].id == source.stages[other].id ||
                    source.stages[index].executionOrder == source.stages[other].executionOrder)
                    return Result<EnvironmentQueryAsset>::Failure(MakeError(AIErrors::EnvironmentQueryIdentityConflict));
        }
        return Result<EnvironmentQueryAsset>::Success(EnvironmentQueryAsset{source});
    }

    /** @copydoc EnvironmentQueryAsset::Id */
    QueryId EnvironmentQueryAsset::Id() const noexcept {
        return source_.id;
    }

    /** @copydoc EnvironmentQueryAsset::ResultSchema */
    const QueryResultSchema &EnvironmentQueryAsset::ResultSchema() const noexcept {
        return source_.result;
    }

    /** @copydoc EnvironmentQueryAsset::Stages */
    std::span<const QueryAssetStage> EnvironmentQueryAsset::Stages() const noexcept {
        return source_.stages;
    }

    /** @copydoc EnvironmentQueryAsset::DisplayName */
    const std::string &EnvironmentQueryAsset::DisplayName() const noexcept {
        return source_.displayName;
    }

    EnvironmentQueryPlan::EnvironmentQueryPlan(const QueryId id, const QueryResultSchema &result, std::vector<QueryPlanStage> stages)
        : id_(id), result_(result), stages_(std::move(stages)) {
        for (const auto &stage : stages_)
            for (const auto &requirement : stage.contexts) {
                const auto found = std::ranges::find_if(requiredContexts_, [&requirement](const auto &entry) {
                    return entry.id == requirement.id;
                });
                if (found == requiredContexts_.end())
                    requiredContexts_.push_back(requirement);
                else {
                    found->version.minimum = std::max(found->version.minimum, requirement.version.minimum);
                    found->version.maximum = std::min(found->version.maximum, requirement.version.maximum);
                }
            }
        std::ranges::sort(requiredContexts_, {}, [](const auto &entry) {
            return entry.id.Value();
        });
    }

    /** @copydoc EnvironmentQueryPlan::Compile */
    Result<EnvironmentQueryPlan> EnvironmentQueryPlan::Compile(const EnvironmentQueryAsset &asset, const QuerySchemaRegistry &registry) {
        const auto *item = registry.Find(asset.ResultSchema().itemType);
        if (item == nullptr)
            return Result<EnvironmentQueryPlan>::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        if (!asset.ResultSchema().itemVersion.Contains(item->origin.version))
            return Result<EnvironmentQueryPlan>::Failure(MakeError(AIErrors::EnvironmentQueryVersionIncompatible));

        std::vector<QueryPlanStage> stages;
        stages.reserve(asset.Stages().size());
        for (const auto &stage : asset.Stages()) {
            auto resolved = ResolveStage(stage, registry, asset.ResultSchema());
            if (resolved.HasError())
                return Result<EnvironmentQueryPlan>::Failure(resolved.ErrorValue());
            stages.push_back(std::move(resolved).Value());
        }
        std::ranges::sort(stages, {}, &QueryPlanStage::executionOrder);
        if (stages.front().kind != QueryStageKind::Generator)
            return Result<EnvironmentQueryPlan>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        return Result<EnvironmentQueryPlan>::Success(EnvironmentQueryPlan{asset.Id(), asset.ResultSchema(), std::move(stages)});
    }

    /** @copydoc EnvironmentQueryPlan::Id */
    QueryId EnvironmentQueryPlan::Id() const noexcept {
        return id_;
    }

    /** @copydoc EnvironmentQueryPlan::ResultSchema */
    const QueryResultSchema &EnvironmentQueryPlan::ResultSchema() const noexcept {
        return result_;
    }

    /** @copydoc EnvironmentQueryPlan::Stages */
    std::span<const QueryPlanStage> EnvironmentQueryPlan::Stages() const noexcept {
        return stages_;
    }

    /** @copydoc EnvironmentQueryPlan::RequiredContexts */
    std::span<const QueryContextRequirement> EnvironmentQueryPlan::RequiredContexts() const noexcept {
        return requiredContexts_;
    }
}  // namespace Horo::AI
