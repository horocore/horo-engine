#pragma once

#include "Horo/Editor/EditorTheme.h"

#include <span>
#include <string>
#include <vector>

namespace Horo::Editor::Ui {
    /** @brief One presentation-only segment in a workflow pipeline. */
    struct BuildPreviewPipelineStep {
        std::string label;
        bool completed{false};
        bool current{false};
        float progress{0.0F};
    };

    /** @brief Draws named, animated pipeline segments above the workflow summary. */
    void BuildPreviewPipeline(std::span<const BuildPreviewPipelineStep> steps, const Theme::Fonts &fonts);

    /** @brief Animated processing marker matching the editor accent color. */
    void BuildPreviewSpinner();

    /** @brief Readable form label for release workflow surfaces. */
    void BuildPreviewFieldLabel(const char *label, const Theme::Fonts &fonts);

    /** @brief Section heading with a trailing rule and optional leading divider. */
    void BuildPreviewSectionHeading(const char *label, const Theme::Fonts &fonts, bool divided);

    /** @brief Bordered informational notice for workflow forms. */
    void BuildPreviewNotice(const char *id, const char *message, const Theme::Fonts &fonts);

    /** @brief One read-only field in a workflow request review. */
    struct BuildPreviewReviewField {
        std::string label;
        std::string value;
    };

    /** @brief Named group of fields in a workflow request review. */
    struct BuildPreviewReviewSection {
        std::string title;
        std::vector<BuildPreviewReviewField> fields;
    };

    /** @brief Draws a bounded, grouped review table with its introduction and notice. */
    void BuildPreviewReview(const char *heading, const char *description, const char *notice,
                            std::span<const BuildPreviewReviewSection> sections, const Theme::Fonts &fonts);
}  // namespace Horo::Editor::Ui
