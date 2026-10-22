#include "editor/screens/workspace/panels/inspector/UiCanvasInspectorPanel.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <limits>

namespace Horo::Editor {
    namespace {
        using namespace Runtime::Ui;

        /** @brief Resolve presentation text without inventing an authored property schema. */
        const char *Label(const EditorGuiContext &context, const char *key) {
            return context.localization.Get("editor", key).c_str();
        }

        /** @brief Capture canonical UUID presentation only on source/input changes. */
        template <typename T> UiInspectorIdText IdText(const T id) {
            UiInspectorIdText text;
            if (id.IsValid()) {
                const auto value = Assets::AssetId::FromBytes(id.Bytes()).ToString();
                std::copy(value.begin(), value.end(), text.bytes.begin());
            }
            return text;
        }

        /** @brief Parse stable typed IDs only on text changes; failed edits preserve the last typed value. */
        template <typename T>
        bool Identity(const EditorGuiContext &context, const char *key, T &value, UiInspectorIdText &text, const bool optional = false) {
            Ui::FieldLabel(Label(context, key), context.theme.fonts);
            if (!Ui::InputTextControl(key, text.bytes.data(), text.bytes.size(), context.theme.fonts, {.error = !text.valid}))
                return false;
            if (optional && text.bytes.front() == '\0') {
                value = {};
                text.valid = true;
                return true;
            }
            const auto parsed = Assets::AssetId::Parse(std::string_view{text.bytes.data()});
            text.valid = parsed.HasValue() && parsed.Value().IsValid();
            if (text.valid) {
                const auto typed = T::Create(parsed.Value().Bytes());
                text.valid = typed.HasValue();
                if (text.valid)
                    value = typed.Value();
            }
            return true;
        }

        /** @brief Labels a cached bounded ID list; ID formatting stays outside unchanged draw frames. */
        bool Select(const EditorGuiContext &context, const char *key, int &index, const std::vector<UiInspectorIdText> &items) {
            Ui::FieldLabel(Label(context, key), context.theme.fonts);
            return Ui::ComboControl(key, &index, static_cast<int>(items.size()),
                                    Ui::ComboItemSource{.label =
                                                            [&items](const int i) {
                return items[static_cast<std::size_t>(i)].bytes.data();
            }},
                                    context.theme.fonts);
        }

        /** @brief Shared localized action without positional offsets or native-widget substitutions. */
        bool Action(const EditorGuiContext &context, const char *key, const bool enabled = true) {
            return Ui::Button({.label = Label(context, key),
                               .enabled = enabled,
                               .font = context.theme.fonts.sans,
                               .style = {.width = Ui::StyleWidth::FillAvailable}});
        }

        /** @brief User-selected canonical literal categories use their actual Runtime UI value types. */
        UiStyleValue Literal(const int category) {
            switch (category) {
                case 0:
                    return UiStyleColor{};
                case 1:
                    return UiStyleDimension{};
                case 2:
                    return UiStyleTypography{};
                case 3:
                    return UiStyleImage{};
                case 4:
                    return UiStyleShape{};
                case 5:
                    return UiStyleScalar{};
                case 6:
                    return UiStyleEnumValue{};
                default:
                    return UiStyleMotionReference{};
            }
        }
    }  // namespace

    /** @copydoc UiCanvasInspectorPanel::Clear */
    void UiCanvasInspectorPanel::Clear() noexcept {
        if (!context_)
            return;
        sourceElement_ = {};
        authoring_ = UiInspectorAuthoringWidgets{};
        context_.reset();
        baseline_ = {};
        presentation_ = {};
        draft_.Clear();
        propertyLabels_.clear();
        classLabels_.clear();
        propertyIndex_ = -1;
        classIndex_ = -1;
        dirty_ = false;
        valid_ = false;
        hadPresentation_ = false;
    }

    /** @copydoc UiCanvasInspectorPanel::Capture */
    void UiCanvasInspectorPanel::Capture(const UiCanvasWorkspaceDocument &source, const UiInspectorContext context) {
        context_ = context;
        sourceElement_ = *source.selectedElement;
        hadPresentation_ = source.selectedElement->presentation.has_value();
        baseline_ = source.selectedElement->presentation.value_or(Runtime::Ui::UiElementPresentation{});
        Reset();
    }

    /** @copydoc UiCanvasInspectorPanel::Reset */
    void UiCanvasInspectorPanel::Reset() {
        presentation_ = baseline_;
        authoring_.Capture(presentation_);
        draft_.Clear();
        styleAsset_ = IdText(presentation_.styleAsset);
        typeAsset_ = IdText(presentation_.typeClass.asset);
        typeClass_ = IdText(presentation_.typeClass.id);
        newProperty_ = {};
        pendingProperty_ = {};
        propertyLabels_.clear();
        classLabels_.clear();
        for (const auto &style : presentation_.inlineStyle)
            propertyLabels_.push_back(IdText(style.property));
        for (const auto &styleClass : presentation_.classes)
            classLabels_.push_back(IdText(styleClass.id));
        propertyIndex_ = -1;
        UiInspectorValues values{.layout = presentation_.layout};
        draft_.Begin(*context_, values);
        UiInspectorWidgets::CaptureLayout(values.layout, layoutText_);
        intrinsicText_[0].Capture(presentation_.intrinsic.fallback.width);
        intrinsicText_[1].Capture(presentation_.intrinsic.fallback.height);
        SelectProperty(propertyLabels_.empty() ? -1 : 0);
        SelectClass(classLabels_.empty() ? -1 : 0);
        dirty_ = false;
        Validate();
    }

    /** @copydoc UiCanvasInspectorPanel::StoreProperty */
    void UiCanvasInspectorPanel::StoreProperty() {
        if (propertyIndex_ >= 0 && draft_.Values().style)
            presentation_.inlineStyle[static_cast<std::size_t>(propertyIndex_)] = *draft_.Values().style;
    }

    /** @copydoc UiCanvasInspectorPanel::SelectProperty */
    void UiCanvasInspectorPanel::SelectProperty(const int index) {
        StoreProperty();
        propertyIndex_ = index;
        UiInspectorValues values{.layout = draft_.Values().layout};
        if (index >= 0)
            values.style = presentation_.inlineStyle[static_cast<std::size_t>(index)];
        draft_.Begin(*context_, values);
        styleText_ = {};
        tokenAsset_ = {};
        tokenId_ = {};
        if (values.style) {
            UiInspectorWidgets::CaptureStyle(values.style->value.literal, styleText_);
            tokenAsset_ = IdText(values.style->value.token.asset);
            tokenId_ = IdText(values.style->value.token.id);
        }
    }

    /** @copydoc UiCanvasInspectorPanel::SelectClass */
    void UiCanvasInspectorPanel::SelectClass(const int index) {
        classIndex_ = index;
        classAsset_ = {};
        classId_ = {};
        if (index >= 0) {
            classAsset_ = IdText(presentation_.classes[static_cast<std::size_t>(index)].asset);
            classId_ = IdText(presentation_.classes[static_cast<std::size_t>(index)].id);
        }
    }

    /** @copydoc UiCanvasInspectorPanel::Validate */
    void UiCanvasInspectorPanel::Validate() {
        StoreProperty();
        const bool styleText = propertyIndex_ < 0 ||
                               (draft_.Values().style->value.referencesToken ? tokenAsset_.valid && tokenId_.valid : styleText_.IsValid());
        valid_ = authoring_.IsValid() && layoutText_.IsValid() && styleText && styleAsset_.valid && typeAsset_.valid && typeClass_.valid &&
                 (classIndex_ < 0 || (classAsset_.valid && classId_.valid)) && intrinsicText_[0].valid && intrinsicText_[1].valid &&
                 draft_.Values().layout.IsValid() && presentation_.IsValid();
        if (valid_) {
            auto candidate = sourceElement_;
            candidate.presentation = presentation_;
            candidate.presentation->layout = draft_.Values().layout;
            valid_ = Runtime::Ui::ValidateCoreUiElement(candidate).HasValue();
        }
    }

    /** @copydoc UiCanvasInspectorPanel::DrawResources */
    bool UiCanvasInspectorPanel::DrawResources(const EditorGuiContext &context) {
        bool changed = Identity(context, "workspace.ui_inspector.style_asset", presentation_.styleAsset, styleAsset_, true);
        changed = Identity(context, "workspace.ui_inspector.type_class_asset", presentation_.typeClass.asset, typeAsset_, true) || changed;
        changed = Identity(context, "workspace.ui_inspector.type_class_id", presentation_.typeClass.id, typeClass_, true) || changed;
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Label(context, "workspace.ui_inspector.resource_resolution"), Theme::Muted(),
                        context.theme.fonts);
        return DrawClasses(context) || changed;
    }

    /** @copydoc UiCanvasInspectorPanel::DrawClasses */
    bool UiCanvasInspectorPanel::DrawClasses(const EditorGuiContext &context) {
        bool changed{};
        if (!classLabels_.empty()) {
            int selected = classIndex_;
            ImGui::BeginDisabled(!classAsset_.valid || !classId_.valid);
            if (Select(context, "workspace.ui_inspector.classes", selected, classLabels_))
                SelectClass(selected);
            ImGui::EndDisabled();
            auto &value = presentation_.classes[static_cast<std::size_t>(classIndex_)];
            changed = Identity(context, "workspace.ui_inspector.class_asset", value.asset, classAsset_);
            if (Identity(context, "workspace.ui_inspector.class_id", value.id, classId_)) {
                if (classId_.valid)
                    classLabels_[static_cast<std::size_t>(classIndex_)] = IdText(value.id);
                changed = true;
            }
            if (Action(context, "workspace.ui_inspector.remove_class")) {
                presentation_.classes.erase(presentation_.classes.begin() + classIndex_);
                classLabels_.erase(classLabels_.begin() + classIndex_);
                SelectClass(classLabels_.empty() ? -1 : 0);
                changed = true;
            }
        }
        if (Action(context, "workspace.ui_inspector.add_class",
                   presentation_.classes.size() < Runtime::Ui::MaximumUiStyleClasses && classAsset_.valid && classId_.valid)) {
            presentation_.classes.push_back({});
            classLabels_.push_back({});
            SelectClass(static_cast<int>(classLabels_.size()) - 1);
            changed = true;
        }
        return changed;
    }

    /** @copydoc UiCanvasInspectorPanel::DrawIntrinsic */
    bool UiCanvasInspectorPanel::DrawIntrinsic(const EditorGuiContext &context) {
        const char *kinds[]{Label(context, "workspace.ui_inspector.intrinsic_none"), Label(context, "workspace.ui_inspector.text"),
                            Label(context, "workspace.ui_inspector.intrinsic_image")};
        int kind = static_cast<int>(presentation_.intrinsic.kind);
        bool changed = Ui::DrawComboPropRow(Label(context, "workspace.ui_inspector.intrinsic_kind"), "intrinsic_kind", kind, kinds,
                                            context.theme.fonts);
        if (changed)
            presentation_.intrinsic.kind = static_cast<Runtime::Ui::UiLayoutIntrinsicKind>(kind);
        changed = Ui::CheckboxControl(Label(context, "workspace.ui_inspector.intrinsic_required"), &presentation_.intrinsic.required,
                                      context.theme.fonts) ||
                  changed;
        std::array<std::int32_t *, 2> values{&presentation_.intrinsic.fallback.width, &presentation_.intrinsic.fallback.height};
        const char *keys[]{"workspace.ui_inspector.fallback_width", "workspace.ui_inspector.fallback_height"};
        for (std::size_t i = 0; i < values.size(); ++i) {
            Ui::FieldLabel(Label(context, keys[i]), context.theme.fonts);
            auto &text = intrinsicText_[i];
            if (Ui::InputTextControl(keys[i], text.bytes.data(), text.bytes.size(), context.theme.fonts, {.error = !text.valid})) {
                static_cast<void>(text.Parse(*values[i]));
                changed = true;
            }
        }
        return changed;
    }

    /** @copydoc UiCanvasInspectorPanel::DrawStyleValue */
    bool UiCanvasInspectorPanel::DrawStyleValue(const EditorGuiContext &context) {
        bool changed{};
        auto &value = draft_.Values().style->value;
        bool token = value.referencesToken;
        if (Ui::CheckboxControl(Label(context, "workspace.ui_inspector.use_token"), &token, context.theme.fonts)) {
            value = token ? Runtime::Ui::UiStyleValueSource::Token({}) : Runtime::Ui::UiStyleValueSource::Literal(value.literal);
            tokenAsset_ = {};
            tokenId_ = {};
            styleText_ = {};
            UiInspectorWidgets::CaptureStyle(value.literal, styleText_);
            changed = true;
        }
        if (token) {
            changed = Identity(context, "workspace.ui_inspector.token_asset", value.token.asset, tokenAsset_) || changed;
            changed = Identity(context, "workspace.ui_inspector.token_id", value.token.id, tokenId_) || changed;
        } else {
            const char *categories[]{Label(context, "workspace.ui_inspector.category_color"),
                                     Label(context, "workspace.ui_inspector.category_dimension"),
                                     Label(context, "workspace.ui_inspector.category_typography"),
                                     Label(context, "workspace.ui_inspector.category_imagery"),
                                     Label(context, "workspace.ui_inspector.category_shape"),
                                     Label(context, "workspace.ui_inspector.category_scalar"),
                                     Label(context, "workspace.ui_inspector.category_enum"),
                                     Label(context, "workspace.ui_inspector.category_motion")};
            int category = static_cast<int>(value.literal.index());
            if (Ui::DrawComboPropRow(Label(context, "workspace.ui_inspector.category"), "category", category, categories,
                                     context.theme.fonts)) {
                value = Runtime::Ui::UiStyleValueSource::Literal(Literal(category));
                UiInspectorWidgets::CaptureStyle(value.literal, styleText_);
                changed = true;
            }
            changed = UiInspectorWidgets::Style(context, value.literal, styleText_) || changed;
        }
        return changed;
    }

    /** @copydoc UiCanvasInspectorPanel::DrawStyle */
    bool UiCanvasInspectorPanel::DrawStyle(const EditorGuiContext &context) {
        bool changed{};
        if (!propertyLabels_.empty()) {
            int selected = propertyIndex_;
            ImGui::BeginDisabled(!styleText_.IsValid() || !tokenAsset_.valid || !tokenId_.valid || !draft_.Values().style->value.IsValid());
            if (Select(context, "workspace.ui_inspector.overrides", selected, propertyLabels_))
                SelectProperty(selected);
            ImGui::EndDisabled();
            changed = DrawStyleValue(context) || changed;
            if (Action(context, "workspace.ui_inspector.remove_override")) {
                presentation_.inlineStyle.erase(presentation_.inlineStyle.begin() + propertyIndex_);
                propertyLabels_.erase(propertyLabels_.begin() + propertyIndex_);
                propertyIndex_ = -1;
                SelectProperty(propertyLabels_.empty() ? -1 : 0);
                changed = true;
            }
        }
        static_cast<void>(Identity(context, "workspace.ui_inspector.property_id", pendingProperty_, newProperty_));
        const bool unique = std::ranges::none_of(presentation_.inlineStyle, [&](const auto &item) {
            return item.property == pendingProperty_;
        });
        if (Action(context, "workspace.ui_inspector.add_override",
                   pendingProperty_.IsValid() && newProperty_.valid && unique &&
                       presentation_.inlineStyle.size() < Runtime::Ui::MaximumUiStyleProperties &&
                       (propertyIndex_ < 0 ||
                        (draft_.Values().style->value.IsValid() && styleText_.IsValid() && tokenAsset_.valid && tokenId_.valid)))) {
            StoreProperty();
            presentation_.inlineStyle.push_back({.property = pendingProperty_});
            propertyLabels_.push_back(IdText(pendingProperty_));
            SelectProperty(static_cast<int>(propertyLabels_.size()) - 1);
            changed = true;
        }
        return changed;
    }

    /** @copydoc UiCanvasInspectorPanel::Emit */
    void UiCanvasInspectorPanel::Emit(const UiCanvasSetPresentation &command, EditorWorkspaceViewCommandData &output) const {
        output.command = EditorWorkspaceViewCommand::UiCanvasApplyPresentation;
        output.documentInstance = context_->document;
        output.uiCanvasState = context_->state;
        output.uiCanvasElement = context_->element;
        output.uiCanvasDocumentRevision = context_->revision;
        output.uiCanvasSelectionRevision = context_->selectionRevision;
        output.uiCanvasInspectorStyles = context_->styles;
        output.uiCanvasInspectorAssets = context_->assets;
        output.uiCanvasInspectorResourcePublication = context_->resourcePublication;
        output.uiCanvasPresentation = command;
    }

    /** @copydoc UiCanvasInspectorPanel::DrawFields */
    bool UiCanvasInspectorPanel::DrawFields(const EditorGuiContext &context, const bool stylesAvailable) {
        Ui::FieldLabel(Label(context, "workspace.ui_inspector.title"), context.theme.fonts);
        bool changed = UiInspectorWidgets::Layout(context, draft_.Values().layout, layoutText_);
        changed = DrawIntrinsic(context) || changed;
        if (!stylesAvailable)
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Label(context, "workspace.ui_inspector.registry_unavailable"), Theme::Muted(),
                            context.theme.fonts);
        ImGui::BeginDisabled(!stylesAvailable);
        changed = DrawResources(context) || changed;
        changed = DrawStyle(context) || changed;
        ImGui::EndDisabled();
        if (const auto *type = Runtime::Ui::UiElementCatalog::Find(sourceElement_.type.Value()))
            changed = authoring_.Draw(context, type->kind, presentation_) || changed;
        return changed;
    }

    /** @copydoc UiCanvasInspectorPanel::DrawTheme */
    bool UiCanvasInspectorPanel::DrawTheme(const UiCanvasWorkspaceDocument &source, const EditorGuiContext &context,
                                           EditorWorkspaceViewCommandData &output) {
        if (!source.inspectorThemes || source.inspectorThemes->empty())
            return false;
        const auto &themes = *source.inspectorThemes;
        const auto selected = std::ranges::find(themes, source.inspectorTheme, &UiInspectorTheme::id);
        int index = selected == themes.end() ? 0 : static_cast<int>(std::distance(themes.begin(), selected)) + 1;
        Ui::FieldLabel(Label(context, "workspace.ui_inspector.project_theme"), context.theme.fonts);
        ImGui::BeginDisabled(source.inspectorStylePending);
        bool changed = Ui::ComboControl("ui_project_theme", &index, static_cast<int>(themes.size()) + 1,
                                        Ui::ComboItemSource{.label =
                                                                [&](const int item) {
            return item == 0 ? Label(context, "workspace.ui_inspector.choose_theme")
                             : themes[static_cast<std::size_t>(item - 1)].label.c_str();
        },
                                                            .enabled =
                                                                [](const int item) {
            return item > 0;
        }},
                                        context.theme.fonts);
        changed = Action(context, "workspace.ui_inspector.refresh_theme", index > 0) || changed;
        ImGui::EndDisabled();
        if (source.inspectorStylePending)
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Label(context, "workspace.ui_inspector.preparing_theme"), Theme::Muted(),
                            context.theme.fonts);
        if (!changed || index <= 0)
            return false;
        output.command = EditorWorkspaceViewCommand::UiCanvasSelectTheme;
        output.documentInstance = source.identity.instance;
        output.uiCanvasState = source.state;
        output.uiCanvasInspectorAssets = source.inspectorAssets;
        output.uiCanvasInspectorResourcePublication = source.inspectorResourcePublication;
        output.uiCanvasTheme = themes[static_cast<std::size_t>(index - 1)].id;
        return true;
    }

    /** @copydoc UiCanvasInspectorPanel::Draw */
    void UiCanvasInspectorPanel::Draw(const UiCanvasWorkspaceDocument &source, const EditorGuiContext &context,
                                      EditorWorkspaceViewCommandData &output) {
        if (DrawTheme(source, context, output))
            return;
        if (!source.selectedElement || source.selectionRevision == std::numeric_limits<std::uint64_t>::max()) {
            Clear();
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Label(context, "workspace.ui_inspector.select_element"), Theme::Muted(),
                            context.theme.fonts);
            return;
        }
        const UiInspectorContext current{.document = source.identity.instance,
                                         .state = source.state,
                                         .revision = source.documentRevision,
                                         .element = source.selection,
                                         .selectionRevision = source.selectionRevision,
                                         .styles = source.inspectorStyles,
                                         .assets = source.inspectorAssets,
                                         .resourcePublication = source.inspectorResourcePublication};
        if (!current.IsValid() || source.selectedElement->id != current.element) {
            Clear();
            Ui::ErrorText(Label(context, "workspace.ui_inspector.invalid_draft"), context.theme.fonts);
            return;
        }
        if (!context_ || *context_ != current)
            Capture(source, current);
        const auto &id = current.element.Bytes();
        ImGui::PushID(reinterpret_cast<const char *>(id.data()), reinterpret_cast<const char *>(id.data() + id.size()));
        const bool changed = DrawFields(context, source.inspectorStyles.IsValid());
        if (changed) {
            dirty_ = true;
            Validate();
        }
        if (dirty_ && !valid_)
            Ui::ErrorText(Label(context, "workspace.ui_inspector.invalid_draft"), context.theme.fonts);
        if (Action(context, "workspace.ui_inspector.apply", dirty_ && valid_)) {
            auto prepared = draft_.PreparePresentation(current, presentation_, true);
            if (prepared.HasValue()) {
                auto command = std::move(prepared).Value();
                if (!hadPresentation_ && command.presentation == baseline_)
                    command.presentation.reset();
                Emit(command, output);
            }
        }
        if (Action(context, "workspace.ui_inspector.cancel", dirty_))
            Reset();
        if (Action(context, "workspace.ui_inspector.reset", hadPresentation_))
            Emit({current.element, std::nullopt}, output);
        ImGui::PopID();
    }
}  // namespace Horo::Editor
