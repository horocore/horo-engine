#pragma once

#include "Horo/AI/DecisionIdentity.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"

#include <algorithm>
#include <array>

namespace Horo::Editor {
    /** @brief Document route and presentation for a registered AI source family. */
    struct AiAssetPresentation final {
        std::string_view type;
        DocumentKind route;
        Ui::UiIcon icon;
    };

    /** @brief Resolves the presentation record for a canonical registered AI asset type. */
    [[nodiscard]] inline const AiAssetPresentation *FindAiAssetPresentation(std::string_view type) noexcept {
        static constexpr std::array families{
            AiAssetPresentation{AI::BlackboardSchemaAssetType, DocumentKind::BlackboardSchema, Ui::UiIcon::AiBlackboard},
            AiAssetPresentation{AI::DecisionAssetType(AI::DecisionPlanKind::BehaviorTree), DocumentKind::DecisionGraph,
                                Ui::UiIcon::AiBehaviorTree},
            AiAssetPresentation{AI::DecisionAssetType(AI::DecisionPlanKind::StateMachine), DocumentKind::DecisionGraph,
                                Ui::UiIcon::AiStateMachine},
            AiAssetPresentation{AI::DecisionAssetType(AI::DecisionPlanKind::Utility), DocumentKind::DecisionGraph, Ui::UiIcon::AiUtility},
            AiAssetPresentation{AI::EnvironmentQuerySourceAssetType, DocumentKind::EnvironmentQuery, Ui::UiIcon::AiEnvironmentQuery},
        };
        const auto found = std::ranges::find(families, type, &AiAssetPresentation::type);
        return found == families.end() ? nullptr : &*found;
    }
}  // namespace Horo::Editor
