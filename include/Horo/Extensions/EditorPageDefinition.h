#pragma once
/** @file EditorPageDefinition.h @brief Inert, restricted settings/modal page schemas backed by canonical configuration types. */
#include "Horo/Extensions/EditorSurfaceDescriptor.h"
#include "Horo/Extensions/EditorUiForm.h"
#include "Horo/Foundation/Configuration.h"

namespace Horo::Extensions {
    inline constexpr std::uint32_t EditorPageSchemaVersion = 1;
    inline constexpr std::size_t MaximumEditorPageFields = 64;

    /** @brief One exact configuration binding. Integer constraints never pass through floating-point UI values. */
    struct EditorPageField final {
        EditorUiBindingId binding;
        SettingDescriptor setting{SettingKey{""},
                                  SettingValueType::Boolean,
                                  false,
                                  SettingScope::User,
                                  ReloadPolicy::NextOperation,
                                  SettingSensitivity::Public,
                                  {}};
        std::optional<std::int64_t> minimum;
        std::optional<std::int64_t> maximum;
        std::size_t maximumStringBytes{4096};
        std::vector<std::string> choices; /**< Empty for free text; otherwise an exact closed string vocabulary. */
    };

    /** @brief Copied provider localization, resolved by the host with explicit en-US fallback. */
    struct EditorPageMessage final {
        std::string locale;
        std::string key;
        std::string value;
    };

    /** @brief Complete inert page schema. Drafts/open state are never workspace persistence data. */
    struct EditorPageDefinition final {
        std::uint32_t schemaVersion{EditorPageSchemaVersion};
        EditorUiForm form;
        std::vector<EditorPageField> fields;
        std::vector<EditorPageMessage> messages;
    };

    /** @brief Validates the complete schema, bindings, exact defaults and localization without activating anything.
     * @param surface Exact inert contribution metadata. @param definition Copied candidate page schema.
     * @return Success or a typed unsupported/invalid surface or configuration error. All inputs remain unchanged.
     * @details V1 supports canonical bool/int64/string with single-line text, integer, toggle, text/label and action controls,
     * plus Stack/Group containers. Controls use Medium/Neutral styling and automatic focus. Row/Grid, path/choice,
     * multiline, decimal/color/vector, confirmation actions and alternate interaction profiles fail closed at registration.
     * Settings fields share one User/Project/Workspace domain; modal fields are Session scoped and never persisted.
     */
    [[nodiscard]] Result<void> ValidateEditorPageDefinition(const EditorSurfaceDescriptor &surface, const EditorPageDefinition &definition);
    /** @brief Validates one draft value against canonical type and exact integer/string constraints.
     * @param field Admitted schema field. @param value Candidate owned value.
     * @return Success or typed invalid-value error; no clamping, lossy conversion, callbacks or persistence.
     */
    [[nodiscard]] Result<void> ValidateEditorPageValue(const EditorPageField &field, const SettingValue &value);
    /** @brief Parses the complete bounded decimal input used by the full-int64 editor.
     * @param text Exact user input, without whitespace or trailing bytes. @return Lossless int64 or typed invalid-value error.
     */
    [[nodiscard]] Result<std::int64_t> ParseEditorPageInteger(std::string_view text);
}  // namespace Horo::Extensions
