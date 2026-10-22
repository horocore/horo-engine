#include "UiInspectorDraft.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Preserve stable Runtime UI error descriptors at the detached consumer boundary. */
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }
    }  // namespace

    /** @copydoc UiInspectorContext::IsValid */
    bool UiInspectorContext::IsValid() const noexcept {
        return document.IsValid() && state.IsValid() && state.instance == document && element.IsValid() && selectionRevision != 0 &&
               revision.IsValid();
    }

    /** @copydoc UiInspectorDraft::Begin */
    bool UiInspectorDraft::Begin(const UiInspectorContext context, const UiInspectorValues &values) {
        if (!context.IsValid()) {
            Clear();
            return true;
        }
        if (context_ == context && baseline_.style.has_value() == values.style.has_value() &&
            (!values.style || baseline_.style->property == values.style->property))
            return false;
        context_ = context;
        baseline_ = values;
        values_ = values;
        return true;
    }

    /** @copydoc UiInspectorDraft::Clear */
    void UiInspectorDraft::Clear() noexcept {
        context_.reset();
        baseline_ = {};
        values_ = {};
    }

    /** @copydoc UiInspectorDraft::Cancel */
    void UiInspectorDraft::Cancel() noexcept {
        values_ = baseline_;
    }

    /** @copydoc UiInspectorDraft::Values */
    UiInspectorValues &UiInspectorDraft::Values() noexcept {
        return values_;
    }

    /** @copydoc UiInspectorDraft::Prepare */
    Result<UiInspectorPreparedEdit> UiInspectorDraft::Prepare(const UiInspectorContext &current,
                                                              const Runtime::Ui::RuntimeStyleRegistry *registry,
                                                              const bool textValid) const {
        using namespace Runtime::Ui;
        if (!context_ || !current.IsValid() || *context_ != current)
            return Failure<UiInspectorPreparedEdit>(UiErrors::RevisionStale);
        if (!values_.layout.IsValid())
            return Failure<UiInspectorPreparedEdit>(UiErrors::LayoutInvalid);
        if (!textValid || values_.style.has_value() != baseline_.style.has_value())
            return Failure<UiInspectorPreparedEdit>(UiErrors::StyleInvalid);
        if (!values_.style)
            return Result<UiInspectorPreparedEdit>::Success({.expected = current, .values = values_});
        if (values_.style->property != baseline_.style->property || values_.style->sealed != baseline_.style->sealed ||
            !values_.style->value.IsValid())
            return Failure<UiInspectorPreparedEdit>(UiErrors::StyleInvalid);
        if (registry == nullptr)
            return Result<UiInspectorPreparedEdit>::Success({.expected = current, .values = values_});
        if (registry->State() != RuntimeStyleRegistryState::Active)
            return Failure<UiInspectorPreparedEdit>(UiErrors::StyleLifecycleUnavailable);
        if (!current.styles.IsValid() || registry->Generation() != current.styles)
            return Failure<UiInspectorPreparedEdit>(UiErrors::RevisionStale);
        const std::array assignments{*values_.style};
        if (const auto valid = Runtime::Ui::ValidateUiStyleAssignments(*registry, assignments); valid.HasError())
            return Result<UiInspectorPreparedEdit>::Failure(valid.ErrorValue());
        return Result<UiInspectorPreparedEdit>::Success({.expected = current, .values = values_});
    }

    /** @copydoc UiInspectorDraft::PreparePresentation */
    Result<UiCanvasSetPresentation> UiInspectorDraft::PreparePresentation(const UiInspectorContext &current,
                                                                          const Runtime::Ui::UiElementPresentation &presentation,
                                                                          const bool textValid) const {
        const auto prepared = Prepare(current, nullptr, textValid);
        if (prepared.HasError())
            return Result<UiCanvasSetPresentation>::Failure(prepared.ErrorValue());
        auto candidate = presentation;
        candidate.layout = prepared.Value().values.layout;
        if (const auto &style = prepared.Value().values.style; style) {
            const auto found = std::ranges::find(candidate.inlineStyle, style->property, &Runtime::Ui::UiStyleAssignment::property);
            if (found == candidate.inlineStyle.end())
                return Failure<UiCanvasSetPresentation>(Runtime::Ui::UiErrors::StyleReferenceInvalid);
            *found = *style;
        }
        if (!candidate.IsValid())
            return Failure<UiCanvasSetPresentation>(Runtime::Ui::UiErrors::StyleInvalid);
        return Result<UiCanvasSetPresentation>::Success({current.element, std::move(candidate)});
    }

    /** @copydoc ExecuteUiInspectorPresentation */
    Result<void> ExecuteUiInspectorPresentation(UiCanvasDocument &owner, const UiInspectorContext &expected,
                                                const std::uint64_t selectionRevision, const UiCanvasSetPresentation &command,
                                                const UiInspectorResourceSnapshot *resources) {
        if (owner.IsClosed())
            return Result<void>::Failure(MakeError(UiCanvasDocumentErrors::Closed));
        if (!expected.IsValid() || owner.Identity().instance != expected.document || owner.CurrentState() != expected.state ||
            owner.Document().Revision() != expected.revision || owner.Selection() != expected.element ||
            selectionRevision != expected.selectionRevision || command.element != expected.element)
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::RevisionStale));
        if (resources) {
            if (resources->publication != expected.resourcePublication)
                return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::AssetRegistryStale));
            if (resources->assets.Revision() != expected.assets)
                return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::AssetRegistryStale));
            if (resources->StyleGeneration() != expected.styles)
                return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::StyleSourceStale));
        } else if (expected.assets.value != 0 || expected.styles.IsValid() || expected.resourcePublication != 0) {
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::StyleLifecycleUnavailable));
        }
        const auto found = std::ranges::find(owner.Document().Elements(), command.element, &Runtime::Ui::UiDocumentElement::id);
        if (found == owner.Document().Elements().end())
            return Result<void>::Failure(MakeError(UiCanvasDocumentErrors::InvalidDocument));
        const Runtime::Ui::UiElementPresentation empty;
        const auto &before = found->presentation ? *found->presentation : empty;
        const auto &after = command.presentation ? *command.presentation : empty;
        const bool changedResources = before.styleAsset != after.styleAsset || before.typeClass != after.typeClass ||
                                      before.classes != after.classes || before.inlineStyle != after.inlineStyle;
        const bool usesResources =
            after.styleAsset.IsValid() || after.typeClass.IsValid() || !after.classes.empty() || !after.inlineStyle.empty();
        const bool changedCascadeState = before.state.InitialVisualState() != after.state.InitialVisualState();
        if ((changedResources || changedCascadeState) && usesResources) {
            if (!resources)
                return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::StyleLifecycleUnavailable));
            if (const auto valid = resources->Validate(owner.Document(), after); valid.HasError())
                return valid;
        }
        return owner.Execute(expected.state, command);
    }
}  // namespace Horo::Editor
