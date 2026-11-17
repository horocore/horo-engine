#pragma once

/**
 * @file SceneConflictCompareModal.h
 * @brief Read-only modal presentation for an external canonical-scene conflict.
 */

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Foundation/JobSystem.h"
#include "editor/document/SceneDocumentComparison.h"

#include <memory>
#include <optional>
#include <string>

namespace Horo::Editor {
    /** @brief Presents a typed, read-only comparison of document and disk scene state. */
    class SceneConflictCompareModal final : public EditorModal {
    public:
        /** @brief Catalog scope shared by this surface and its view helpers. */
        static constexpr GuiLocalizationScope kTextScope{"workspace.scene_compare"};

        static constexpr std::uint64_t kModalId = 0x53434E434D504152ULL;

        /**
         * @brief Creates a comparison modal from an immutable conflict snapshot.
         * @param context Borrowed editor presentation context.
         * @param comparison Owning typed comparison projection.
         */
        SceneConflictCompareModal(const EditorGuiContext &context, JobSystem &jobs, SceneDocumentComparisonRequest request);
        /** @brief Cancels and joins accepted comparison work before releasing modal state. */
        ~SceneConflictCompareModal() override;

        [[nodiscard]] ModalId Id() const override;
        [[nodiscard]] ModalPresentation Presentation() const override;
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &context) override;
        void OnUpdate(float dt) override;
        [[nodiscard]] ModalFrameResult Draw() override;
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason reason) override;

    private:
        struct Completion;
        const EditorGuiContext &context_;
        JobSystem &jobs_;
        SceneDocumentComparisonRequest request_;
        std::string absoluteScenePath_;
        CancellationSource cancellation_;
        std::optional<JobHandle> job_;
        std::shared_ptr<Completion> completion_;
        std::optional<SceneDocumentComparison> comparison_;
        std::optional<Error> error_;
    };
}  // namespace Horo::Editor
