#include "Horo/Runtime/Ui/UiElementCatalog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        constexpr std::array Types{
            UiElementTypeDescriptor{UiElementKind::Canvas, "horo.ui.canvas", "workspace.ui_canvas.kind.canvas", true, true},
            UiElementTypeDescriptor{UiElementKind::Screen, "horo.ui.screen", "workspace.ui_canvas.kind.screen", true, true},
            UiElementTypeDescriptor{UiElementKind::Panel, "horo.ui.panel", "workspace.ui_canvas.kind.panel", true, true},
            UiElementTypeDescriptor{UiElementKind::Frame, "horo.ui.frame", "workspace.ui_canvas.kind.frame", true, true},
            UiElementTypeDescriptor{UiElementKind::LayoutGroup, "horo.ui.layout_group", "workspace.ui_canvas.kind.layout_group", true,
                                    true},
            UiElementTypeDescriptor{UiElementKind::Text, "horo.ui.text", "workspace.ui_canvas.kind.text", false, false},
            UiElementTypeDescriptor{UiElementKind::Image, "horo.ui.image", "workspace.ui_canvas.kind.image", false, false},
            UiElementTypeDescriptor{UiElementKind::Button, "horo.ui.button", "workspace.ui_canvas.kind.button", true, false},
            UiElementTypeDescriptor{UiElementKind::ProgressBar, "horo.ui.progress_bar", "workspace.ui_canvas.kind.progress_bar", false,
                                    false},
            UiElementTypeDescriptor{UiElementKind::Slider, "horo.ui.slider", "workspace.ui_canvas.kind.slider", true, false},
            UiElementTypeDescriptor{UiElementKind::Toggle, "horo.ui.toggle", "workspace.ui_canvas.kind.toggle", true, false},
            UiElementTypeDescriptor{UiElementKind::TextInput, "horo.ui.text_input", "workspace.ui_canvas.kind.text_input", false, false},
            UiElementTypeDescriptor{UiElementKind::ScrollView, "horo.ui.scroll_view", "workspace.ui_canvas.kind.scroll_view", true, false},
            UiElementTypeDescriptor{UiElementKind::CollectionView, "horo.ui.collection_view", "workspace.ui_canvas.kind.collection_view",
                                    true, false},
        };

        /** @brief Returns the existing document-schema error for a malformed core descriptor. */
        template <typename T = void> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(UiErrors::DocumentSerializationInvalid));
        }

    }  // namespace

    /** @copydoc UiElementCatalog::All */
    std::span<const UiElementTypeDescriptor> UiElementCatalog::All() noexcept {
        return Types;
    }

    /** @copydoc UiElementCatalog::Find(UiElementKind) */
    const UiElementTypeDescriptor *UiElementCatalog::Find(const UiElementKind kind) noexcept {
        const auto found = std::ranges::find(Types, kind, &UiElementTypeDescriptor::kind);
        return found == Types.end() ? nullptr : &*found;
    }

    /** @copydoc UiElementCatalog::Find(std::string_view) */
    const UiElementTypeDescriptor *UiElementCatalog::Find(const std::string_view type) noexcept {
        const auto found = std::ranges::find(Types, type, &UiElementTypeDescriptor::documentType);
        return found == Types.end() ? nullptr : &*found;
    }

    /** @copydoc CreateUiElement */
    Result<UiDocumentElement> CreateUiElement(const UiElementKind kind, const UiElementId id, const UiElementId parent) {
        const auto *descriptor = UiElementCatalog::Find(kind);
        if (!descriptor || !id.IsValid() || (kind == UiElementKind::Canvas) == parent.IsValid() || id == parent)
            return Invalid<UiDocumentElement>();
        auto type = Assets::AssetTypeId::Parse(descriptor->documentType);
        if (type.HasError())
            return Result<UiDocumentElement>::Failure(type.ErrorValue());
        UiElementPresentation presentation;
        if (kind == UiElementKind::Text || kind == UiElementKind::TextInput)
            presentation.intrinsic.kind = UiLayoutIntrinsicKind::Text;
        if (kind == UiElementKind::Image)
            presentation.intrinsic.kind = UiLayoutIntrinsicKind::Image;
        return Result<UiDocumentElement>::Success({id, parent, std::move(type).Value(), {}, {}, std::move(presentation)});
    }

    /** @copydoc ValidateCoreUiElement */
    Result<void> ValidateCoreUiElement(const UiDocumentElement &element) {
        if (!element.type.Value().starts_with("horo.ui."))
            return Result<void>::Success();
        const auto *type = UiElementCatalog::Find(element.type.Value());
        if (!type || !element.id.IsValid() || (type->kind == UiElementKind::Canvas) == element.parent.IsValid() ||
            element.properties.size() > MaximumUiDocumentProperties)
            return Invalid();
        if (!element.properties.empty())
            return Invalid();
        if (element.presentation) {
            if (!element.presentation->IsValid())
                return Invalid();
            const auto &initial = element.presentation->state.initial;
            if ((std::holds_alternative<bool>(initial) && type->kind != UiElementKind::Toggle) ||
                (std::holds_alternative<UiControlScalarValues>(initial) && type->kind != UiElementKind::Slider) ||
                (std::holds_alternative<std::string>(initial) && type->kind != UiElementKind::TextInput) ||
                (element.presentation->localizedText && type->kind != UiElementKind::Text))
                return Invalid();
            if (element.presentation->accessibility) {
                const auto role = element.presentation->accessibility->role;
                if ((role == UiAccessibilityRole::Slider && type->kind != UiElementKind::Slider) ||
                    (role == UiAccessibilityRole::TextField && type->kind != UiElementKind::TextInput) ||
                    ((role == UiAccessibilityRole::Toggle || role == UiAccessibilityRole::Checkbox || role == UiAccessibilityRole::Radio) &&
                     type->kind != UiElementKind::Toggle))
                    return Invalid();
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc ProjectUiElementLayout */
    Result<UiLayoutElementDescriptor> ProjectUiElementLayout(const UiDocumentElement &element, const UiElementHandle handle) {
        if (!handle.IsValid() || !UiElementCatalog::Find(element.type.Value()) || ValidateCoreUiElement(element).HasError())
            return Invalid<UiLayoutElementDescriptor>();
        const UiElementPresentation defaults;
        const auto &presentation = element.presentation ? *element.presentation : defaults;
        return Result<UiLayoutElementDescriptor>::Success({handle, presentation.layout, presentation.intrinsic});
    }

    /** @copydoc ProjectUiElementStyle */
    Result<UiStyleElementInput> ProjectUiElementStyle(const UiDocumentElement &element, const UiElementHandle handle) {
        if (!handle.IsValid() || !UiElementCatalog::Find(element.type.Value()) || ValidateCoreUiElement(element).HasError())
            return Invalid<UiStyleElementInput>();
        UiStyleElementInput result{.element = handle};
        if (element.presentation) {
            result.state = element.presentation->state.InitialVisualState();
            result.asset = element.presentation->styleAsset;
            result.typeClass = element.presentation->typeClass;
            result.classes = element.presentation->classes;
            result.inlineProperties = element.presentation->inlineStyle;
        }
        return Result<UiStyleElementInput>::Success(result);
    }

    /** @copydoc ProjectUiElementSurface */
    Result<UiThemeSurfaceInput> ProjectUiElementSurface(const UiDocumentElement &element, const UiElementHandle handle) {
        const auto *type = UiElementCatalog::Find(element.type.Value());
        if (!type || !type->surface || !handle.IsValid() || ValidateCoreUiElement(element).HasError())
            return Invalid<UiThemeSurfaceInput>();
        auto layout = ProjectUiElementLayout(element, handle);
        auto style = ProjectUiElementStyle(element, handle);
        if (layout.HasError() || style.HasError())
            return Invalid<UiThemeSurfaceInput>();
        UiThemeSurfaceInput result;
        result.style = style.Value();
        result.layout = layout.Value().style;
        result.intrinsic = layout.Value().intrinsic;
        result.fill = {0, 0, 0, 0};
        result.visible = !element.presentation || element.presentation->state.visible;
        return Result<UiThemeSurfaceInput>::Success(std::move(result));
    }
}  // namespace Horo::Runtime::Ui
