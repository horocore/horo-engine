#include "DecisionCompileDetail.h"

#include <algorithm>

namespace Horo::AI::Detail {
    /** @copydoc ValidUtilitySourceBounds */
    bool ValidUtilitySourceBounds(const UtilityDecisionSource &source) noexcept {
        return source.root.IsValid() && !source.options.empty() && source.options.size() <= UtilitySelector::MaximumOptions &&
               source.requirements.size() <= DecisionAssetValidationHardLimits::RequirementsPerNode &&
               std::ranges::all_of(source.options, [](const auto &option) {
            return option.option.considerations.size() <= UtilitySelector::MaximumConsiderations &&
                   option.properties.size() <= BehaviorTreeAssetHardLimits::PropertiesPerNode &&
                   option.provider.requirements.size() <= DecisionAssetValidationHardLimits::RequirementsPerNode;
        });
    }

    /** @copydoc LowerUtility */
    Result<void> LowerUtility(LoweredDecisionGraph &graph, const UtilityDecisionSource &source, const SourceLocation &location) {
        if (!ValidUtilitySourceBounds(source))
            return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));

        graph.metadata.nodes.push_back(
            {.id = source.root, .requirements = source.requirements, .source = location, .intrinsic = DecisionControlKind::Root});
        for (const auto &option : source.options) {
            if (option.option.id != option.provider.id || option.provider.intrinsic != DecisionControlKind::Unspecified)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
            auto provider = option.provider;
            if (provider.source.source.empty())
                provider.source = location;
            graph.metadata.nodes.push_back(std::move(provider));
            graph.properties.push_back({option.option.id, option.properties});
        }
        graph.utility = source;
        return Result<void>::Success();
    }
}  // namespace Horo::AI::Detail
