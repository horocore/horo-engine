#include "Horo/Extensions/EditorUiForm.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Extensions {
    /** @copydoc EditorUiFormBuilder::EditorUiFormBuilder */
    EditorUiFormBuilder::EditorUiFormBuilder(EditorUiForm form, const EditorUiFormLimits &limits) noexcept
        : form_(std::move(form)), limits_(limits) {}

    /** @copydoc EditorUiFormBuilder::Add */
    Result<void> EditorUiFormBuilder::Add(EditorUiNode node) {
        if (form_.nodes.size() >= limits_.maximumNodes)
            return Result<void>::Failure(
                MakeError(ExtensionErrors::EditorUiFormCapacityExceeded, "Editor UI form node capacity is exhausted."));
        if (const Result<void> validation = ValidateEditorUiNode(node, limits_); validation.HasError())
            return validation;
        if (const EditorUiNodeBase &base = EditorUiNodeBaseOf(node); std::ranges::find_if(form_.nodes, [&](const EditorUiNode &existing) {
            return EditorUiNodeBaseOf(existing).id == base.id;
        }) != form_.nodes.end())
            return Result<void>::Failure(
                MakeError(ExtensionErrors::EditorUiFormInvalid, "Editor UI node identities must be unique within a form."));
        try {
            form_.nodes.push_back(std::move(node));
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(
                MakeError(ExtensionErrors::EditorUiFormCapacityExceeded, "Editor UI form node storage could not be extended."));
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorUiFormBuilder::AddLabel */
    Result<void> EditorUiFormBuilder::AddLabel(EditorUiLabelNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddText */
    Result<void> EditorUiFormBuilder::AddText(EditorUiTextNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddTextField */
    Result<void> EditorUiFormBuilder::AddTextField(EditorUiTextFieldNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddNumber */
    Result<void> EditorUiFormBuilder::AddNumber(EditorUiNumberNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddBoolean */
    Result<void> EditorUiFormBuilder::AddBoolean(EditorUiBooleanNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddChoice */
    Result<void> EditorUiFormBuilder::AddChoice(EditorUiChoiceNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddPath */
    Result<void> EditorUiFormBuilder::AddPath(EditorUiPathNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddColor */
    Result<void> EditorUiFormBuilder::AddColor(EditorUiColorNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddVector */
    Result<void> EditorUiFormBuilder::AddVector(EditorUiVectorNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddAction */
    Result<void> EditorUiFormBuilder::AddAction(EditorUiActionNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddValidation */
    Result<void> EditorUiFormBuilder::AddValidation(EditorUiValidationNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddContainer */
    Result<void> EditorUiFormBuilder::AddContainer(EditorUiContainerNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::AddHelp */
    Result<void> EditorUiFormBuilder::AddHelp(EditorUiHelpNode node) {
        return Add(EditorUiNode{std::move(node)});
    }

    /** @copydoc EditorUiFormBuilder::Build */
    Result<EditorUiForm> EditorUiFormBuilder::Build() && {
        if (Result<void> validation = ValidateEditorUiForm(form_, limits_); validation.HasError())
            return Result<EditorUiForm>::Failure(std::move(validation).ErrorValue());
        return Result<EditorUiForm>::Success(std::move(form_));
    }
}  // namespace Horo::Extensions
