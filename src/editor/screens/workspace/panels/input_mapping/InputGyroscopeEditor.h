#pragma once

/** @file InputGyroscopeEditor.h @brief Target-private gyro draft and exclusive modal over the canonical Input profile. */
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <array>

namespace Horo::Editor {
    /** @brief Owned intermediate text; no change reaches the router before explicit Apply. */
    struct GyroscopeBindingEditorDraft final {
        int axis{1};
        int component{};
        std::array<char, 32> calibration{};
        std::array<char, 32> sensitivity{};
        /** @brief Copies exact finite float text from an existing binding. @param binding Original binding.
         * @return Independent draft; the existing deadzone/modifier policy is preserved separately by the request baseline. */
        [[nodiscard]] static GyroscopeBindingEditorDraft FromBinding(const Input::InputBinding &binding) noexcept;
        /** @brief Parses complete dot/comma decimal fields, retaining invalid intermediate drafts unchanged.
         * @param baseline Binding whose unrelated fields must be preserved. @return Complete typed gyro binding or invalid-draft error. */
        [[nodiscard]] Result<Input::InputBinding> Evaluate(const Input::InputBinding &baseline) const;
    };

    /** @brief Appends or replaces through canonical profile validation after checking exact action/revision/baseline identity.
     * @param router Authoritative input owner. @param request Owned intent captured at the user's open gesture.
     * @param draft Current editable desired binding. @return Original validation failure or success; rejection leaves all bindings
     * unchanged. */
    [[nodiscard]] Result<void> ApplyGyroscopeBindingDraft(Input::InputRouter &router, const GyroscopeBindingEditRequest &request,
                                                          const GyroscopeBindingEditorDraft &draft);
    /** @brief Creates an exclusive host-owned modal; it borrows only process GUI/Input services, never a panel or action vector.
     * @param context Stable GUI services outliving the modal host. @param router Input owner outliving the modal host.
     * @param request Owned revision-fenced intent. @return Unopened modal for the existing EditorModalHost. */
    [[nodiscard]] std::unique_ptr<EditorModal> MakeGyroscopeBindingModal(const EditorGuiContext &context, Input::InputRouter &router,
                                                                         GyroscopeBindingEditRequest request);
}  // namespace Horo::Editor
