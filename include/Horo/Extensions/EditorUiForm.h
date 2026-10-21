#pragma once

/**
 * @file EditorUiForm.h
 * @brief Backend-neutral standard components and declarative editor forms for extensions.
 */

#include "Horo/Extensions/EditorThemeTokens.h"
#include "Horo/Foundation/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Horo::Extensions {
    inline constexpr std::uint32_t EditorUiFormSchemaVersion = 1;
    inline constexpr std::size_t MaximumEditorUiVectorComponents = 4;

    /** @brief Stable identity shared by one declarative form node and its host snapshot. */
    struct EditorUiId final {
        std::string value;

        [[nodiscard]] bool IsValid() const noexcept {
            return !value.empty();
        }

        bool operator==(const EditorUiId &) const noexcept = default;
    };

    /** @brief Stable identity of a value binding owned by a configuration or capability service. */
    struct EditorUiBindingId final {
        std::string value;

        [[nodiscard]] bool IsValid() const noexcept {
            return !value.empty();
        }

        bool operator==(const EditorUiBindingId &) const noexcept = default;
    };

    /** @brief Stable identity of a typed action routed by the host to an extension capability. */
    struct EditorUiActionId final {
        std::string value;

        [[nodiscard]] bool IsValid() const noexcept {
            return !value.empty();
        }

        bool operator==(const EditorUiActionId &) const noexcept = default;
    };

    /** @brief Whether text is resolved by localization or deliberately technical. */
    enum class EditorUiTextKind : std::uint8_t {
        LocalizationKey,
        TechnicalText,
    };

    /**
     * @brief Owned user-visible text intent; the host performs localization and shaping.
     *
     * Ordinary labels, descriptions, help, and validation messages should use a
     * localization key. Technical text is reserved for values such as an asset
     * identifier or a compiler-provided diagnostic fragment.
     */
    struct EditorUiText final {
        EditorUiTextKind kind{EditorUiTextKind::LocalizationKey};
        std::string value;
    };

    /** @brief Shared control geometry role resolved from host theme metrics. */
    enum class EditorUiComponentSize : std::uint8_t {
        Small,
        Medium,
        Large,
    };

    /** @brief Semantic visual emphasis; it never names or stores a color. */
    enum class EditorUiSemanticTone : std::uint8_t {
        Neutral,
        Primary,
        Secondary,
        Positive,
        Warning,
        Critical,
    };

    /** @brief Focus policy used by the host's keyboard traversal graph. */
    enum class EditorUiFocusPolicy : std::uint8_t {
        Automatic,
        Never,
    };

    /** @brief Typed validation severity displayed by a validation primitive. */
    enum class EditorUiValidationSeverity : std::uint8_t {
        Info,
        Warning,
        Error,
    };

    /** @brief Bounded normalized color value edited by a color field; this is domain data, not theme styling. */
    struct EditorUiColorValue final {
        float red{};
        float green{};
        float blue{};
        float alpha{1.0F};

        /** @brief Checks finite normalized channel values. @return True when the value is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Bounded 2D, 3D, or 4D numeric value edited by a vector field. */
    struct EditorUiVectorValue final {
        std::array<double, MaximumEditorUiVectorComponents> components{};
        std::uint8_t dimension{3};

        /** @brief Checks the dimension and finite active components. @return True when the value is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Closed value vocabulary used in host-routed value-change requests. */
    using EditorUiValue = std::variant<bool, std::int64_t, double, std::string, EditorUiColorValue, EditorUiVectorValue>;

    /** @brief Closed advanced presentation catalog; extensions cannot register executable painters. */
    enum class EditorUiCustomWidget : std::uint8_t {
        Standard,
        ScalarDial
    };

    /** @brief Host-clipped semantic presentation shared by custom compositions, not a drawing command stream. */
    struct EditorUiCustomPresentation final {
        std::uint32_t schemaVersion{1};
        EditorUiCustomWidget widget{EditorUiCustomWidget::Standard};
        EditorThemeFontRole font{EditorThemeFontRole::Sans};
        EditorThemeIconRole icon{EditorThemeIconRole::None};
        EditorThemeColorRole accent{EditorThemeColorRole::Accent};
        EditorThemeMotionRole motion{EditorThemeMotionRole::Focus};
        std::uint8_t controlRows{3}; /**< Theme-relative custom surface height, bounded to1..8 rows. */
    };

    /** @brief Host-owned finite resource policy. Custom providers receive no callbacks, subscriptions or native resources. */
    struct EditorUiCompositionBudget final {
        std::size_t maximumCustomWidgets{16};
        std::size_t maximumPlotSamples{4096};
        std::size_t maximumPresentationStateBytes{4096};
        std::size_t maximumDrawSegments{8192};
    };

    /** @brief Common identity, text, state, and accessibility metadata for one form node. */
    struct EditorUiNodeBase final {
        EditorUiId id;                /**< Stable node identity; it is never derived only from visible text. */
        EditorUiId parent;            /**< Optional parent container; an empty value means the form root. */
        EditorUiText label;           /**< Visible label or container title. */
        EditorUiText description;     /**< Optional supporting text. */
        EditorUiText accessibleLabel; /**< Optional accessible name; the host may derive it from label. */
        EditorUiComponentSize size{EditorUiComponentSize::Medium};
        EditorUiSemanticTone tone{EditorUiSemanticTone::Neutral};
        EditorUiFocusPolicy focusPolicy{EditorUiFocusPolicy::Automatic};
        bool enabled{true};                               /**< Disabled nodes remain visible but cannot receive input. */
        bool readOnly{};                                  /**< Value fields may display a value without accepting edits. */
        std::optional<EditorUiCustomPresentation> custom; /**< Versioned host presentation request; no authority or resources. */
    };

    /** @brief Display-only label primitive. */
    struct EditorUiLabelNode final {
        EditorUiNodeBase base;
        EditorUiText text;
    };

    /** @brief Display-only text primitive for bounded technical or localized content. */
    struct EditorUiTextNode final {
        EditorUiNodeBase base;
        EditorUiText text;
    };

    /** @brief Single-line or multiline text field bound to an owning value authority. */
    struct EditorUiTextFieldNode final {
        EditorUiNodeBase base;
        EditorUiBindingId binding;
        std::string value;
        EditorUiText placeholder;
        std::size_t maximumBytes{4U * 1024U};
        bool multiline{};
    };

    /** @brief Numeric field kind exposed by the standard form kit. */
    enum class EditorUiNumberKind : std::uint8_t {
        Integer,
        Decimal,
    };

    /** @brief Numeric field with optional host-rendered range and step hints. */
    struct EditorUiNumberNode final {
        EditorUiNodeBase base;
        EditorUiBindingId binding;
        EditorUiNumberKind numberKind{EditorUiNumberKind::Decimal};
        std::variant<std::int64_t, double> value{0.0};
        std::optional<double> minimum;
        std::optional<double> maximum;
        std::optional<double> step;
    };

    /** @brief Boolean field bound to an owning value authority. */
    struct EditorUiBooleanNode final {
        EditorUiNodeBase base;
        EditorUiBindingId binding;
        bool value{};
    };

    /** @brief One typed option in a choice field. */
    struct EditorUiChoiceOption final {
        std::string id;
        EditorUiText label;
        bool enabled{true};
    };

    /** @brief Closed choice field with stable option identities. */
    struct EditorUiChoiceNode final {
        EditorUiNodeBase base;
        EditorUiBindingId binding;
        std::string selected;
        std::vector<EditorUiChoiceOption> options;
        bool allowEmpty{};
    };

    /** @brief Path selection mode; the host owns dialogs and path policy. */
    enum class EditorUiPathKind : std::uint8_t {
        File,
        Directory,
    };

    /** @brief Path field whose value is handed to an owning capability for policy validation. */
    struct EditorUiPathNode final {
        EditorUiNodeBase base;
        EditorUiBindingId binding;
        std::string value;
        EditorUiPathKind pathKind{EditorUiPathKind::File};
        bool allowRelative{true};
    };

    /** @brief Color field for extension or project data; it cannot alter host theme colors. */
    struct EditorUiColorNode final {
        EditorUiNodeBase base;
        EditorUiBindingId binding;
        EditorUiColorValue value;
        bool allowAlpha{true};
    };

    /** @brief Vector field dimension and value. */
    struct EditorUiVectorNode final {
        EditorUiNodeBase base;
        EditorUiBindingId binding;
        EditorUiVectorValue value;
    };

    /** @brief Semantic action emphasis for an action primitive. */
    enum class EditorUiActionKind : std::uint8_t {
        Primary,
        Secondary,
        Destructive,
    };

    inline constexpr std::size_t MaximumEditorUiPlotSamples = 256;

    /** @brief Normalized plot data; x strictly increases and both coordinates remain in [0,1]. */
    struct EditorUiPlotSample final {
        float x{};
        float y{};
    };

    /** @brief Specialized plot composition. The host owns drawing, selection, focus and activation.
     * @details Samples are copied outside draw traversal. Activation emits only the declared action;
     * it cannot supply document edits. Selected samples are transient host presentation state.
     */
    struct EditorUiPlotNode final {
        EditorUiNodeBase base;
        EditorUiActionId action;
        std::vector<EditorUiPlotSample> samples;
    };

    /** @brief Bounded local selection and animation, discarded with the owning surface generation. */
    struct EditorUiPlotState final {
        std::size_t selected{};
        float emphasis{};
    };

    /** @brief Host-scoped semantic input; no global key or platform event crosses the boundary. */
    enum class EditorUiPlotInput : std::uint8_t {
        None,
        Previous,
        Next,
        Select,
        Activate
    };

    /** @brief Updates local presentation without allocation, callbacks or document access.
     * @param plot Validated owned plot. @param state Host-owned transient state.
     * @param input Scoped semantic input. @param normalizedX Host hit-tested normalized coordinate.
     * @param admitted Host focus/modal/provider gate. @param theme Current immutable theme frame.
     * @param elapsedSeconds Finite frame duration, clamped to the motion duration.
     * @return True only for an admitted activation; the caller must still use command admission.
     */
    [[nodiscard]] bool UpdateEditorUiPlot(const EditorUiPlotNode &plot, EditorUiPlotState &state, EditorUiPlotInput input,
                                          float normalizedX, bool admitted, const EditorThemeFrame &theme, float elapsedSeconds) noexcept;

    /** @brief Typed action primitive; invocation is routed by action identity, never by callback. */
    struct EditorUiActionNode final {
        EditorUiNodeBase base;
        EditorUiActionId action;
        EditorUiActionKind actionKind{EditorUiActionKind::Secondary};
        bool requiresConfirmation{};
    };

    /** @brief Validation message primitive supplied by the owning capability or configuration service. */
    struct EditorUiValidationNode final {
        EditorUiNodeBase base;
        EditorUiValidationSeverity severity{EditorUiValidationSeverity::Info};
        std::string code;
        EditorUiText message;
    };

    /** @brief Layout/container vocabulary shared by all form adapters. */
    enum class EditorUiLayoutKind : std::uint8_t {
        Group,
        Stack,
        Row,
        Grid,
    };

    /** @brief Grouping or responsive layout primitive. Children are referenced by parent identity. */
    struct EditorUiContainerNode final {
        EditorUiNodeBase base;
        EditorUiLayoutKind layout{EditorUiLayoutKind::Stack};
        std::uint8_t columns{1};
    };

    /** @brief Help primitive for bounded supporting guidance. */
    struct EditorUiHelpNode final {
        EditorUiNodeBase base;
        EditorUiText text;
    };

    /** @brief Closed standard-component vocabulary admitted by schema version one. */
    using EditorUiNodePayload =
        std::variant<EditorUiLabelNode, EditorUiTextNode, EditorUiTextFieldNode, EditorUiNumberNode, EditorUiBooleanNode,
                     EditorUiChoiceNode, EditorUiPathNode, EditorUiColorNode, EditorUiVectorNode, EditorUiActionNode,
                     EditorUiValidationNode, EditorUiContainerNode, EditorUiHelpNode, EditorUiPlotNode>;

    /** @brief One typed node in a declarative extension form. */
    struct EditorUiNode final {
        EditorUiNodePayload payload;
    };

    /** @brief Closed node kinds used in render snapshots and accessibility projections. */
    enum class EditorUiNodeKind : std::uint8_t {
        Label,
        Text,
        TextField,
        Number,
        Boolean,
        Choice,
        Path,
        Color,
        Vector,
        Action,
        Validation,
        Group,
        Stack,
        Row,
        Grid,
        Help,
        Plot,
    };

    /**
     * @brief Returns the closed node kind represented by a typed payload.
     * @param node Node whose payload is inspected.
     * @return Stable schema kind used by GUI and non-GUI adapters.
     */
    [[nodiscard]] EditorUiNodeKind EditorUiNodeKindOf(const EditorUiNode &node) noexcept;

    /**
     * @brief Returns the common metadata of a typed node.
     * @param node Node whose shared metadata is requested.
     * @return Borrowed common metadata owned by the node.
     */
    [[nodiscard]] const EditorUiNodeBase &EditorUiNodeBaseOf(const EditorUiNode &node) noexcept;

    /** @brief Resource bounds applied before a form reaches any host adapter. */
    struct EditorUiFormLimits final {
        std::size_t maximumFormIdentityBytes{128};
        std::size_t maximumNodeIdentityBytes{128};
        std::size_t maximumBindingIdentityBytes{128};
        std::size_t maximumActionIdentityBytes{128};
        std::size_t maximumLocalizationKeyBytes{256};
        std::size_t maximumTextBytes{4U * 1024U};
        std::size_t maximumNodes{256};
        std::size_t maximumDepth{16};
        std::size_t maximumChoices{64};
        std::size_t maximumChoiceLabelBytes{256};
        std::size_t maximumValidationMessages{32};
        EditorUiCompositionBudget composition;
    };

    /** @brief Complete owned form schema; value persistence remains outside this projection. */
    struct EditorUiForm final {
        std::uint32_t schemaVersion{EditorUiFormSchemaVersion};
        EditorUiId id;
        EditorUiText title;
        EditorUiText description;
        std::vector<EditorUiNode> nodes;
    };

    /** @brief Resolves inherited enabled state without callbacks, allocation or host access.
     * @param form Copied form. @param id Stable node identity to resolve.
     * @return False for missing/cyclic/over-depth ancestry or any disabled node/ancestor.
     */
    [[nodiscard]] bool IsEditorUiNodeEnabled(const EditorUiForm &form, const EditorUiId &id) noexcept;

    /**
     * @brief Validates one typed node without registering, rendering, or invoking extension code.
     * @param node Candidate standard-component node.
     * @param limits Host-owned bounds.
     * @return Success or Extensions::EditorUiFormInvalid.
     */
    [[nodiscard]] Result<void> ValidateEditorUiNode(const EditorUiNode &node, const EditorUiFormLimits &limits = {});

    /**
     * @brief Validates an entire form tree and its stable field/action identities.
     * @param form Owned declarative form candidate.
     * @param limits Host-owned bounds.
     * @return Success or a typed form/schema rejection.
     * @post No host, service, renderer, callback, or persistence authority is touched.
     */
    [[nodiscard]] Result<void> ValidateEditorUiForm(const EditorUiForm &form, const EditorUiFormLimits &limits = {});

    /**
     * @brief Incrementally composes a form from the public standard primitives.
     *
     * The builder owns all copied nodes. Add methods do not retain extension
     * references or callbacks; the final form is immutable by convention after
     * it has been accepted by a host.
     */
    class EditorUiFormBuilder final {
    public:
        /**
         * @brief Creates a builder with a stable form identity and localized title.
         * @param id Form identity.
         * @param title Localized form title.
         * @param limits Resource bounds applied to additions and final validation.
         * @return Builder or a typed validation/capacity error.
         */
        [[nodiscard]] static Result<EditorUiFormBuilder> Create(EditorUiId id, EditorUiText title, const EditorUiFormLimits &limits = {});

        EditorUiFormBuilder(const EditorUiFormBuilder &) = delete;
        EditorUiFormBuilder &operator=(const EditorUiFormBuilder &) = delete;
        EditorUiFormBuilder(EditorUiFormBuilder &&) noexcept = default;
        EditorUiFormBuilder &operator=(EditorUiFormBuilder &&) noexcept = default;

        /** @brief Adds one generic typed node from the public primitive vocabulary. */
        [[nodiscard]] Result<void> Add(EditorUiNode node);
        [[nodiscard]] Result<void> AddLabel(EditorUiLabelNode node);
        [[nodiscard]] Result<void> AddText(EditorUiTextNode node);
        [[nodiscard]] Result<void> AddTextField(EditorUiTextFieldNode node);
        [[nodiscard]] Result<void> AddNumber(EditorUiNumberNode node);
        [[nodiscard]] Result<void> AddBoolean(EditorUiBooleanNode node);
        [[nodiscard]] Result<void> AddChoice(EditorUiChoiceNode node);
        [[nodiscard]] Result<void> AddPath(EditorUiPathNode node);
        [[nodiscard]] Result<void> AddColor(EditorUiColorNode node);
        [[nodiscard]] Result<void> AddVector(EditorUiVectorNode node);
        [[nodiscard]] Result<void> AddAction(EditorUiActionNode node);
        [[nodiscard]] Result<void> AddValidation(EditorUiValidationNode node);
        [[nodiscard]] Result<void> AddContainer(EditorUiContainerNode node);
        [[nodiscard]] Result<void> AddHelp(EditorUiHelpNode node);

        /**
         * @brief Finishes composition and returns the owned validated form.
         * @return Form or a typed validation/capacity error.
         * @pre The builder is an rvalue so ownership is transferred exactly once.
         */
        [[nodiscard]] Result<EditorUiForm> Build() &&;

    private:
        EditorUiFormBuilder(EditorUiForm form, const EditorUiFormLimits &limits) noexcept;

        EditorUiForm form_;
        EditorUiFormLimits limits_;
    };

    /** @brief Compatibility alias for the color roles used by standard editor forms. */
    using EditorUiThemeToken = EditorThemeColorRole;

    /** @brief Compatibility alias for the shared DPI-aware geometry token set. */
    using EditorUiThemeMetrics = EditorThemeSizeTokens;

    /** @brief Compatibility alias for the complete versioned frame-scoped theme contract. */
    using EditorUiThemeFrame = EditorThemeFrame;

    /** @brief Returns the semantic color-role bit used by EditorUiThemeFrame::supportedTokenMask. */
    [[nodiscard]] constexpr std::uint64_t EditorUiThemeTokenBit(const EditorUiThemeToken token) noexcept {
        return EditorThemeColorRoleBit(token);
    }

    /**
     * @brief Maps a requested semantic token through the host's supported-token fallback policy.
     * @param requested Semantic role requested by the standard component.
     * @param frame Immutable host theme evidence.
     * @return Requested token when supported, or a safe semantic fallback.
     */
    [[nodiscard]] EditorUiThemeToken ResolveEditorUiThemeToken(EditorUiThemeToken requested, const EditorUiThemeFrame &frame) noexcept;

    /** @brief Host-owned scalar gesture state. Draft preview never becomes document state without a routed commit. */
    struct EditorUiScalarState final {
        double preview{};
        float emphasis{};
        bool editing{};
        std::uint64_t lastPresentedFrame{}; /**< Host continuity fence; not a document revision or authority token. */
    };

    /** @brief Semantic custom interaction vocabulary, scoped by the owning host's focus/modal and provider context. */
    enum class EditorUiScalarInput : std::uint8_t {
        None,
        Begin,
        Preview,
        Commit,
        Cancel,
        Increment,
        Decrement
    };

    /** @brief Applies a bounded scalar gesture without allocation, callbacks or domain mutation.
     * @param node Admitted decimal dial with finite range. @param state Host transient state.
     * @param input Scoped semantic gesture. @param normalizedValue Host hit-tested position in[0,1].
     * @param admitted Exact provider/focus/modal gate. @param theme Current immutable semantic frame.
     * @param elapsedSeconds Frame duration. @return A proposed scalar only on commit; the owner must revalidate and execute its command.
     */
    [[nodiscard]] std::optional<double> UpdateEditorUiScalar(const EditorUiNumberNode &node, EditorUiScalarState &state,
                                                             EditorUiScalarInput input, double normalizedValue, bool admitted,
                                                             const EditorThemeFrame &theme, float elapsedSeconds) noexcept;

    /** @brief Closed semantic accessibility roles exposed equally to GUI, tests and future native adapters. */
    enum class EditorUiAccessibleRole : std::uint8_t {
        Text,
        Group,
        Action,
        Field,
        Plot,
        ScalarDial
    };

    /** @brief Copied accessibility record; localized names remain typed intents resolved by the owning host. */
    struct EditorUiAccessibleNode final {
        EditorUiId id;
        EditorUiId parent;
        EditorUiText name;
        EditorUiText description;
        EditorUiAccessibleRole role{EditorUiAccessibleRole::Text};
        bool enabled{};
        bool readOnly{};
        bool focusable{};
        std::uint32_t focusOrder{};
        std::optional<EditorUiValue> value;
        std::uint32_t itemCount{}; /**< Bounded plot sample count; zero for other roles. */
        std::optional<std::uint32_t> selectedIndex;
        bool preview{}; /**< Local uncommitted presentation value, never authored state. */
    };

    /** @brief Validates one custom presentation request without performing host admission.
     * @param node Candidate node and presentation metadata. @return Success or typed unsupported form rejection.
     */
    [[nodiscard]] Result<void> ValidateEditorUiCustomPresentation(const EditorUiNode &node);

    /** @brief Reports whether a form requests any specialized drawing or explicit custom presentation.
     * @param form Copied semantic graph. @return True when the custom-composition capability is required at registration and use.
     */
    [[nodiscard]] bool RequiresEditorUiComposition(const EditorUiForm &form) noexcept;
    /** @brief Validates shared custom presentation, resource budgets and closed widget/type compatibility.
     * @param form Copied graph. @param budget Host-owned finite bounds. @return Success or a typed form/capacity rejection.
     */
    [[nodiscard]] Result<void> ValidateEditorUiComposition(const EditorUiForm &form, const EditorUiCompositionBudget &budget = {});

    /** @brief One backend-neutral render record shared by editor and headless adapters.
     * @details Bounds allocate the complete node, including field captions and container headers;
     * action labels are inside their control. Adapters clip content to this allocation.
     */
    struct EditorUiRenderNode final {
        EditorUiId id;
        EditorUiId parent;
        EditorUiNodeKind kind{EditorUiNodeKind::Text};
        EditorUiComponentSize size{EditorUiComponentSize::Medium};
        EditorUiSemanticTone tone{EditorUiSemanticTone::Neutral};
        EditorUiThemeToken foregroundToken{EditorUiThemeToken::TextPrimary};
        EditorUiThemeToken backgroundToken{EditorUiThemeToken::None};
        EditorUiThemeToken borderToken{EditorUiThemeToken::None};
        bool enabled{true};
        bool readOnly{};
        bool focusable{};
        std::uint32_t focusOrder{};
        float x{};
        float y{};
        float width{};
        float height{};
        EditorThemeFontRole font{EditorThemeFontRole::Sans};
        EditorThemeIconRole icon{EditorThemeIconRole::None};
        EditorThemeColorRole accent{EditorThemeColorRole::Accent};
        EditorThemeMotionRole motion{EditorThemeMotionRole::None};
    };

    /** @brief Deterministic render projection with semantic styles and scaled layout geometry. */
    struct EditorUiRenderSnapshot final {
        std::uint32_t schemaVersion{EditorUiFormSchemaVersion};
        EditorUiId form;
        std::uint64_t themeRevision{};
        float uiScale{1.0F};
        float availableWidth{};
        EditorThemeFrame theme;
        std::vector<EditorUiRenderNode> nodes;
        std::vector<EditorUiAccessibleNode> accessibility; /**< Complete stable semantic tree, without a native accessibility claim. */
    };

    /** @brief Builds one stable semantic accessibility record from the canonical form and render geometry.
     * @param node Copied node. @param render Exact current resolved geometry/state for that node.
     * @return Owned semantic metadata; no native accessibility support is implied.
     */
    [[nodiscard]] EditorUiAccessibleNode BuildEditorUiAccessibleNode(const EditorUiNode &node, const EditorUiRenderNode &render);

    /**
     * @brief Projects one validated form into a deterministic adapter-neutral render snapshot.
     * @param form Declarative form owned by the extension host.
     * @param theme Immutable frame-scoped theme and UI-scale evidence.
     * @param availableWidth Logical width offered by the current editor surface.
     * @return Render snapshot or a typed form/theme rejection.
     * @note No GUI toolkit, renderer, localization service, persistence store, or extension callback is accessed.
     */
    [[nodiscard]] Result<EditorUiRenderSnapshot> BuildEditorUiRenderSnapshot(const EditorUiForm &form, const EditorUiThemeFrame &theme,
                                                                             float availableWidth = 0.0F);
}  // namespace Horo::Extensions
