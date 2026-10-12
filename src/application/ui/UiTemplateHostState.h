#pragma once

#include "Horo/Application/UiTemplateHost.h"

namespace Horo::Application {
    namespace Detail {
        /** @brief Owned semantic action metadata; no call-duration text view survives capture. */
        struct UiTemplateSemanticAction final {
            Runtime::Ui::UiAccessibilityActionInput descriptor;
            std::string name;
        };

        /** @brief Complete semantic declaration, including hidden nodes needed by post-reconciliation extraction. */
        struct UiTemplateSemanticNode final {
            Runtime::Ui::UiAccessibilityNodeInput descriptor;
            std::string name;
            std::string description;
            std::string value;
            std::string error;
            std::vector<Runtime::Ui::UiAccessibilityRelationInput> relations;
            std::vector<UiTemplateSemanticAction> actions;
        };

        /** @brief Owned load-time schema inputs retained independently of application adapter lifetimes. */
        struct UiTemplateSemanticSource final {
            Runtime::Ui::UiCanvasId canvas;
            Runtime::Ui::UiAccessibilityExtractorDescriptor owner;
            std::vector<UiTemplateSemanticNode> nodes;
        };

        /** @brief Captures complete declarations with actual arranged bounds and resolved-text assignments. */
        [[nodiscard]] Result<UiTemplateSemanticSource> CaptureTemplateSemantics(const Runtime::Ui::UiReloadCanvas &canvas,
                                                                                const UiTemplateCanvasComposition &input,
                                                                                std::span<const UiTemplateResolvedText> texts);
        /** @brief Derives owned semantic/focus evidence from the actual initial or reconciled candidate. */
        [[nodiscard]] Result<Runtime::Ui::UiAccessibilitySnapshot> ExtractTemplateSemantics(
            const Runtime::Ui::UiReloadCanvas &canvas, const UiTemplateSemanticSource &source,
            Runtime::Ui::UiAccessibilitySemanticRevision revision);
    }  // namespace Detail

    /** @brief Real capture and existing verified package authority retained independently of later publications. */
    struct UiTemplateInputLease::State final {
        Assets::AssetCookInputSnapshot sources;
        Assets::AssetRegistrySnapshot registry;
        std::shared_ptr<const Packages::PackageRestoreGraph> packages;
        std::string packageIdentity;
    };

    /** @brief Complete owned runtime projection and every exact authority that justified its resource manifest. */
    struct UiTemplateHostCook::State final {
        UiTemplates::UiTemplateCookedDocument document;
        UiTemplates::UiTemplateDocumentSnapshot source;
        std::vector<UiTemplateInputLease> inputs;
        UiTemplateResourceBindings resources;
        std::vector<UiTemplateCanvasProjection> projections;
        std::vector<Detail::UiTemplateSemanticSource> semanticSources;
    };
}  // namespace Horo::Application
