#include "HoroScriptAuthoringConfig.h"

#include "Horo/Assets/AssetSourcePublication.h"
#include "SourceSyntax.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace Horo::Editor::Language {
    namespace {
        using Json = nlohmann::json;
        using Bindings = std::vector<ScriptModuleSourceBinding>;
        using Output = Result<Bindings>;
        constexpr std::size_t MaximumConfigBytes = 64 * 1024;
        constexpr std::size_t MaximumBindings = 128;
        const ErrorCodeDescriptor InvalidConfig{ErrorDomainId{"editor.script_language"},
                                                ErrorCode{"script_language_invalid_config"},
                                                ErrorSeverity::Warning,
                                                "Invalid project script-language configuration.",
                                                {}};

        /** @brief Borrows one field with a finite, control-free string representation. */
        std::string_view Field(const Json &record, const char *key, std::size_t limit) {
            const auto value = record.find(key);
            if (value == record.end() || !value->is_string())
                return {};
            const auto &text = value->get_ref<const std::string &>();
            if (text.size() > limit || std::ranges::any_of(text, [](unsigned char byte) {
                return byte < 0x20 || byte == 0x7f;
            }))
                return {};
            return text;
        }

        /** @brief Validates one portable source path before any host lookup. */
        std::optional<ScriptModuleSourceBinding> DecodeBinding(const Json &record) {
            auto source = SourceDocumentId::Parse(Field(record, "source", 1024));
            const auto package = Field(record, "package", 256);
            const auto module = Field(record, "module", 256);
            if (source.HasError() || package.empty() || module.empty())
                return std::nullopt;
            if (ClassifySourceLanguage(source.Value().Value()) != SourceLanguage::HoroScript)
                return std::nullopt;
            return ScriptModuleSourceBinding{std::move(source).Value(), std::string{package}, std::string{module}};
        }

        /** @brief Decodes a complete versioned binding set and rejects ambiguous sources. */
        Output DecodeBindings(const std::string_view text) {
            const auto parsed = Json::parse(text, nullptr, false);
            if (!parsed.is_object() || !parsed.contains("schemaVersion") || parsed["schemaVersion"] != 1)
                return Output::Failure(MakeError(InvalidConfig));
            const auto records = parsed.find("bindings");
            if (records == parsed.end() || !records->is_array() || records->size() > MaximumBindings)
                return Output::Failure(MakeError(InvalidConfig));
            Bindings bindings;
            bindings.reserve(records->size());
            for (const auto &record : *records) {
                auto binding = DecodeBinding(record);
                if (!binding)
                    return Output::Failure(MakeError(InvalidConfig));
                bindings.push_back(std::move(*binding));
            }
            std::ranges::sort(bindings, {}, &ScriptModuleSourceBinding::source);
            if (std::ranges::adjacent_find(bindings, {}, &ScriptModuleSourceBinding::source) != bindings.end())
                return Output::Failure(MakeError(InvalidConfig, "The same source selects more than one script module."));
            return Output::Success(std::move(bindings));
        }
    }  // namespace

    /** @copydoc LoadScriptModuleSourceBindings */
    Output LoadScriptModuleSourceBindings(const std::filesystem::path &root) {
        const auto capture = Assets::CaptureAssetSource(root, root / ".horo/script-language.json", MaximumConfigBytes);
        if (capture.HasError())
            return Output::Failure(capture.ErrorValue());
        if (!capture.Value().identity.exists)
            return Output::Success({});
        const auto &bytes = capture.Value().bytes;
        if (bytes.empty())
            return Output::Failure(MakeError(InvalidConfig));
        return DecodeBindings({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
    }

    /** @copydoc FindScriptModuleSourceBinding */
    const ScriptModuleSourceBinding *FindScriptModuleSourceBinding(const std::span<const ScriptModuleSourceBinding> bindings,
                                                                   const SourceDocumentId &source) noexcept {
        const auto found = std::ranges::lower_bound(bindings, source, {}, &ScriptModuleSourceBinding::source);
        return found != bindings.end() && found->source == source ? &*found : nullptr;
    }
}  // namespace Horo::Editor::Language
