#include "HoroScriptModuleLanguage.h"

#include "LuaModuleHostApi.h"
#include "ScriptModuleHostServices.h"
#include "SourceSyntax.h"

#include <algorithm>

namespace Horo::Editor::Language {
    namespace {
        constexpr std::size_t MaximumExpressionBytes = 4096;
        constexpr std::size_t MaximumChoices = 128;
        constexpr std::size_t MaximumImportDiagnostics = 64;

        /** @brief Engine-owned fields on the import result envelope. */
        struct ImportStatusField final {
            std::string_view name;
            std::string_view signature;
        };

        constexpr std::array ImportStatusFields{
            ImportStatusField{Extensions::LuaModuleImportAvailable, "available: boolean"},
            ImportStatusField{Extensions::LuaModuleImportErrorCode, "code: string?"},
        };

        /** @brief Matches a byte that can occur inside an ASCII Lua identifier. */
        bool Identifier(const char value) noexcept {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '_';
        }

        /** @brief Discards Lua whitespace before the next delimiter. */
        void SkipWhitespace(std::string_view &text) {
            const auto first = text.find_first_not_of(" \t\r\n\v\f");
            text.remove_prefix(first == text.npos ? text.size() : first);
        }

        /** @brief Consumes one grammar terminal at a token boundary. */
        bool Take(std::string_view &text, std::string_view token) {
            SkipWhitespace(text);
            if (!text.starts_with(token))
                return false;
            text.remove_prefix(token.size());
            return true;
        }

        /** @brief Reads one unescaped Lua string used as a manifest alias. */
        std::optional<std::string_view> ReadLiteralAlias(std::string_view &text) {
            SkipWhitespace(text);
            if (text.empty() || (text.front() != '"' && text.front() != '\''))
                return std::nullopt;
            const char quote = text.front();
            text.remove_prefix(1);
            const auto end = text.find(quote);
            const auto alias = text.substr(0, end);
            if (end == text.npos || alias.find_first_of("\\\r\n") != alias.npos)
                return std::nullopt;
            text.remove_prefix(end + 1);
            return alias;
        }

        /** @brief Reads the exact import call without evaluating the expression. */
        std::optional<std::string_view> ReadAlias(std::string_view &text) {
            if (!Take(text, Extensions::LuaModuleHostReceiver) || !Take(text, ".") || !Take(text, Extensions::LuaModuleHostImportName) ||
                !Take(text, "("))
                return std::nullopt;
            const auto alias = ReadLiteralAlias(text);
            return alias && Take(text, ")") ? alias : std::nullopt;
        }

        /** @brief Reads the dotted namespace preceding the requested member. */
        std::optional<std::string> ReadNamespace(std::string_view text) {
            std::string path;
            while (Take(text, ".")) {
                SkipWhitespace(text);
                if (text.empty())
                    return path;
                const auto end = std::ranges::find_if_not(text, Identifier);
                if (end == text.begin())
                    return std::nullopt;
                if (!path.empty())
                    path += '.';
                const auto size = static_cast<std::size_t>(end - text.begin());
                path.append(text.substr(0, size));
                text.remove_prefix(size);
            }
            return std::nullopt;
        }

        /** @brief Locates the full identifier without splitting its replacement at the caret. */
        LanguageByteSpan MemberSpan(const std::string_view text, std::size_t offset) noexcept {
            std::size_t first = offset;
            while (first > 0 && Identifier(text[first - 1]))
                --first;
            while (offset < text.size() && Identifier(text[offset]))
                ++offset;
            return {first, offset};
        }

        /** @brief A direct host expression. An empty alias identifies the engine-owned host table. */
        struct ImportMember final {
            std::string_view alias;
            std::string path;
            LanguageByteSpan member;
        };

        /** @brief Refuses textual matches in comments, strings, or qualified receivers. */
        bool HostReceiverAt(const std::string_view text, const SourceSyntax &syntax, const std::size_t start) {
            if (syntax.RoleAt(start) != SourceSyntaxRole::Text)
                return false;
            const auto previous = text.substr(0, start).find_last_not_of(" \t\r\n\v\f");
            if (previous == text.npos)
                return true;
            return text[previous] != '.' && text[previous] != ':' && (previous + 1 != start || !Identifier(text[previous]));
        }

        /** @brief Searches a finite window for the closest lexical receiver preceding the caret. */
        std::optional<std::size_t> FindHostReceiver(const std::string_view text, const SourceSyntax &syntax, const std::size_t before) {
            const auto lower = before > MaximumExpressionBytes ? before - MaximumExpressionBytes : 0;
            const auto window = text.substr(lower, before - lower);
            auto start = window.rfind(Extensions::LuaModuleHostReceiver);
            while (start != window.npos) {
                if (HostReceiverAt(text, syntax, lower + start))
                    return lower + start;
                if (start == 0)
                    break;
                start = window.rfind(Extensions::LuaModuleHostReceiver, start - 1);
            }
            return std::nullopt;
        }

        /** @brief Matches a member of either the engine host table or one imported namespace. */
        std::optional<ImportMember> FindHostMember(const std::string_view text, const SourceSyntax &syntax, const std::size_t offset) {
            const auto member = MemberSpan(text, offset);
            const auto left = text.substr(0, member.start);
            const auto start = FindHostReceiver(text, syntax, member.start);
            if (!start)
                return std::nullopt;
            auto expression = left.substr(*start);
            const auto alias = ReadAlias(expression);
            if (alias && !alias->empty()) {
                if (auto path = ReadNamespace(expression))
                    return ImportMember{*alias, std::move(*path), member};
                return std::nullopt;
            }
            expression = left.substr(*start + std::string_view{Extensions::LuaModuleHostReceiver}.size());
            if (!Take(expression, "."))
                return std::nullopt;
            SkipWhitespace(expression);
            return expression.empty() ? std::optional{ImportMember{{}, {}, member}} : std::nullopt;
        }

        /** @brief Locates the complete quoted alias content, excluding both quotes. */
        std::optional<LanguageByteSpan> FindAliasSpan(const std::string_view text, const SourceSyntax &syntax, const std::size_t offset) {
            if (syntax.RoleAt(offset) != SourceSyntaxRole::String)
                return std::nullopt;
            const auto start = FindHostReceiver(text, syntax, offset);
            if (!start)
                return std::nullopt;
            auto expression = text.substr(*start, MaximumExpressionBytes);
            const auto alias = ReadAlias(expression);
            if (!alias)
                return std::nullopt;
            const auto first = static_cast<std::size_t>(alias->data() - text.data());
            const auto end = first + alias->size();
            return offset >= first && offset <= end ? std::optional{LanguageByteSpan{first, end}} : std::nullopt;
        }

        /** @brief Produces an inert callable signature from the actual schema parameters. */
        std::string FormatSignature(const Extensions::ScriptExportFunctionDescriptor &function) {
            std::string signature = function.id;
            signature += '(';
            for (const auto &parameter : function.parameters) {
                if (signature.back() != '(')
                    signature += ", ";
                signature += parameter.id;
            }
            signature += ')';
            return signature;
        }

        /** @brief Appends host-supplied documentation as plain text. */
        std::string Describe(std::string label, const Extensions::ScriptExportDocumentation &documentation) {
            for (const auto *text : {&documentation.summary, &documentation.description}) {
                if (!text->empty()) {
                    label += '\n';
                    label += *text;
                }
            }
            return label;
        }

        /** @brief Appends one prefix-matching completion, retaining the original replacement range. */
        void AddChoice(LanguageCompletions &completion, const LanguageByteSpan span, const std::string_view prefix,
                       const std::string_view name, std::string detail = {}) {
            if (name.empty() || !name.starts_with(prefix))
                return;
            if (completion.items.size() == MaximumChoices) {
                completion.incomplete = true;
                return;
            }
            completion.items.push_back({.label = std::string{name},
                                        .detail = std::move(detail),
                                        .insertText = std::string{name},
                                        .edits = {{span, std::string{name}}},
                                        .primaryEdit = span});
        }

        /** @brief Offers only names representable as Lua identifiers in a dotted expression. */
        void AddIdentifierChoice(LanguageCompletions &completion, const LanguageByteSpan span, const std::string_view prefix,
                                 const std::string_view name, std::string detail = {}) {
            if (std::ranges::all_of(name, Identifier))
                AddChoice(completion, span, prefix, name, std::move(detail));
        }

        /** @brief Resolves the next namespace segment or actual published members. */
        LanguageCompletions Complete(const Extensions::ScriptExportDescriptor &descriptor, const ImportMember &member,
                                     const std::string_view prefix) {
            LanguageCompletions completion;
            const std::string_view nameSpace = descriptor.nameSpace;
            if (nameSpace == member.path) {
                for (const auto &function : descriptor.functions)
                    AddIdentifierChoice(completion, member.member, prefix, function.id, FormatSignature(function));
                for (const auto &constant : descriptor.constants)
                    AddIdentifierChoice(completion, member.member, prefix, constant.id, constant.documentation.summary);
                std::ranges::sort(completion.items, {}, &LanguageCompletionItem::label);
            } else if (member.path.empty()) {
                AddIdentifierChoice(completion, member.member, prefix, nameSpace.substr(0, nameSpace.find('.')));
            } else if (nameSpace.starts_with(member.path) && nameSpace[member.path.size()] == '.') {
                const auto suffix = nameSpace.substr(member.path.size() + 1);
                AddIdentifierChoice(completion, member.member, prefix, suffix.substr(0, suffix.find('.')));
            }
            return completion;
        }

        /** @brief Describes the exact imported function or constant at the caret. */
        LanguagePayload DescribeMember(const Extensions::ScriptExportDescriptor &descriptor, const ImportMember &member,
                                       const std::string_view token) {
            if (descriptor.nameSpace != member.path)
                return std::monostate{};
            const auto function = std::ranges::find(descriptor.functions, token, &Extensions::ScriptExportFunctionDescriptor::id);
            if (function != descriptor.functions.end())
                return LanguageHover{Describe(FormatSignature(*function), function->documentation), member.member};
            const auto constant = std::ranges::find(descriptor.constants, token, &Extensions::ScriptExportConstantDescriptor::id);
            if (constant != descriptor.constants.end())
                return LanguageHover{Describe(constant->id, constant->documentation), member.member};
            return std::monostate{};
        }

        /** @brief Projects the engine-owned host catalog without consulting a VM or an installed module. */
        LanguagePayload ProjectHostMembers(const SourceLanguageRequestKind kind, const LanguageByteSpan span, const std::string_view name) {
            if (kind == SourceLanguageRequestKind::Completion) {
                LanguageCompletions completion;
                for (const auto &member : Extensions::LuaModuleHostMembers)
                    AddChoice(completion, span, name, member.name, member.signature);
                return completion;
            }
            const auto found = std::ranges::find(Extensions::LuaModuleHostMembers, name, &Extensions::LuaModuleHostMember::name);
            if (found == Extensions::LuaModuleHostMembers.end())
                return std::monostate{};
            return LanguageHover{found->signature, span};
        }

        /** @brief Describes or replaces only the unescaped content of a quoted import alias. */
        LanguagePayload ProjectAlias(const SourceLanguageRequestKind kind, const LanguageByteSpan span, const std::string_view name,
                                     const std::span<const Extensions::ScriptModuleApiMetadata> imports) {
            if (kind == SourceLanguageRequestKind::Completion) {
                LanguageCompletions completion;
                for (const auto &import : imports) {
                    if (import.descriptor && import.alias.find_first_of("'\"\\\r\n") == import.alias.npos)
                        AddChoice(completion, span, name, import.alias, import.descriptor->nameSpace);
                }
                std::ranges::sort(completion.items, {}, &LanguageCompletionItem::label);
                return completion;
            }
            const auto found = std::ranges::find(imports, name, &Extensions::ScriptModuleApiMetadata::alias);
            if (found == imports.end() || !found->descriptor)
                return std::monostate{};
            return LanguageHover{Describe(found->descriptor->nameSpace, found->descriptor->documentation), span};
        }

        /** @brief Merges namespace completion with the status fields defined on the import result. */
        LanguageCompletions CompleteImportedMember(const ImportMember &member, const std::string_view prefix,
                                                   const Extensions::ScriptModuleApiMetadata &import, const std::string_view root) {
            auto completion = import.descriptor ? Complete(*import.descriptor, member, prefix) : LanguageCompletions{};
            if (!member.path.empty())
                return completion;
            for (const auto &[field, signature] : ImportStatusFields) {
                if (field != root)
                    AddChoice(completion, member.member, prefix, field, std::string{signature});
            }
            std::ranges::sort(completion.items, {}, &LanguageCompletionItem::label);
            return completion;
        }

        /** @brief Projects the registered namespace and the runtime-owned import availability fields. */
        LanguagePayload ProjectImportedMember(const SourceLanguageRequestKind kind, const ImportMember &member, const std::string_view name,
                                              const Extensions::ScriptModuleApiMetadata &import) {
            const std::string_view nameSpace = import.descriptor ? std::string_view{import.descriptor->nameSpace} : std::string_view{};
            const auto root = nameSpace.substr(0, nameSpace.find('.'));
            if (kind == SourceLanguageRequestKind::Completion)
                return CompleteImportedMember(member, name, import, root);
            if (member.path.empty() && name != root) {
                const auto field = std::ranges::find(ImportStatusFields, name, &ImportStatusField::name);
                if (field != ImportStatusFields.end())
                    return LanguageHover{std::string{field->signature}, member.member};
            }
            return import.descriptor ? DescribeMember(*import.descriptor, member, name) : LanguagePayload{std::monostate{}};
        }

        /** @brief Resolves a lexical host expression and queries its engine or admitted module catalog. */
        LanguagePayload ProjectApi(const std::string_view text, const SourceLanguageRequestKind kind, const std::size_t offset,
                                   const std::span<const Extensions::ScriptModuleApiMetadata> imports) {
            const auto syntax = TokenizeSource(SourceLanguage::HoroScript, text);
            if (syntax.fallback != SourceSyntaxFallback::None)
                return std::monostate{};
            if (const auto alias = FindAliasSpan(text, syntax, offset)) {
                const auto end = kind == SourceLanguageRequestKind::Completion ? offset : alias->end;
                return ProjectAlias(kind, *alias, text.substr(alias->start, end - alias->start), imports);
            }
            const auto member = FindHostMember(text, syntax, offset);
            if (!member)
                return std::monostate{};
            const auto end = kind == SourceLanguageRequestKind::Completion ? offset : member->member.end;
            const auto name = text.substr(member->member.start, end - member->member.start);
            if (member->alias.empty())
                return ProjectHostMembers(kind, member->member, name);
            const auto imported = std::ranges::find(imports, member->alias, &Extensions::ScriptModuleApiMetadata::alias);
            if (imported == imports.end())
                return std::monostate{};
            return ProjectImportedMember(kind, *member, name, *imported);
        }

        /** @brief Checks an exported terminal symbol without resolving or invoking its runtime value. */
        bool HasExportedMember(const Extensions::ScriptExportDescriptor &descriptor, const std::string_view member) {
            return std::ranges::find(descriptor.functions, member, &Extensions::ScriptExportFunctionDescriptor::id) !=
                       descriptor.functions.end() ||
                   std::ranges::find(descriptor.constants, member, &Extensions::ScriptExportConstantDescriptor::id) !=
                       descriptor.constants.end();
        }

        /** @brief Returns the first member token that disagrees with the published namespace or symbol table. */
        std::string_view UnknownMember(std::string_view expression, const Extensions::ScriptExportDescriptor &descriptor) {
            std::string_view expected = descriptor.nameSpace;
            bool atImportRoot = true;
            while (Take(expression, ".")) {
                SkipWhitespace(expression);
                const auto end = std::ranges::find_if_not(expression, Identifier);
                const auto member = expression.substr(0, static_cast<std::size_t>(end - expression.begin()));
                if (member.empty())
                    return {};
                expression.remove_prefix(member.size());
                const auto separator = expected.find('.');
                const auto segment = expected.substr(0, separator);
                if (atImportRoot && member != segment &&
                    std::ranges::find(ImportStatusFields, member, &ImportStatusField::name) != ImportStatusFields.end())
                    return {};  // The envelope field is valid. Its downstream uses are outside schema validation.
                atImportRoot = false;
                if (expected.empty())
                    return HasExportedMember(descriptor, member) ? std::string_view{} : member;
                if (member != segment)
                    return member;
                expected = separator == expected.npos ? std::string_view{} : expected.substr(separator + 1);
            }
            return {};
        }

        /** @brief Checks an eligible lexical receiver without evaluating Lua or guessing at unavailable imports. */
        std::optional<LanguageDiagnostic> DiagnoseImport(const std::string_view text, const std::size_t start,
                                                         const std::span<const Extensions::ScriptModuleApiMetadata> imports) {
            auto expression = text.substr(start, MaximumExpressionBytes);
            const auto alias = ReadAlias(expression);
            if (!alias)
                return std::nullopt;
            const auto imported = std::ranges::find(imports, *alias, &Extensions::ScriptModuleApiMetadata::alias);
            if (imported == imports.end() || !imported->descriptor)
                return std::nullopt;
            const auto token = UnknownMember(expression, *imported->descriptor);
            if (token.empty() ||
                (token.data() + token.size() == expression.data() + expression.size() && start + MaximumExpressionBytes < text.size()))
                return std::nullopt;
            const auto first = static_cast<std::size_t>(token.data() - text.data());
            return LanguageDiagnostic{{first, first + token.size()},
                                      LanguageSeverity::Warning,
                                      {},
                                      LanguageDiagnosticCode::UnknownScriptImportMember};
        }
    }  // namespace

    /** @copydoc QueryHoroScriptApi */
    Result<LanguageResult> QueryHoroScriptApi(const SourceDocumentSnapshot &source, const SourceLanguageRequestKind kind,
                                              const std::size_t offset,
                                              const std::span<const Extensions::ScriptModuleApiMetadata> imports) {
        using Output = Result<LanguageResult>;
        using enum SourceLanguageRequestKind;
        if (!source.IsCurrent())
            return Output::Failure(MakeError(SourceDocumentErrors::Stale));
        const auto text = source.Text();
        if ((kind != Completion && kind != Hover) || offset > text.size() ||
            ClassifySourceLanguage(source.Identity().key.source.Value()) != SourceLanguage::HoroScript)
            return Output::Failure(MakeError(SourceDocumentErrors::Invalid));
        return Output::Success({{kind, source}, ProjectApi(text, kind, offset, imports)});
    }

    /** @copydoc DiagnoseHoroScriptImports */
    LanguageDiagnostics DiagnoseHoroScriptImports(const std::string_view text,
                                                  const std::span<const Extensions::ScriptModuleApiMetadata> imports) {
        LanguageDiagnostics diagnostics;
        if (imports.empty())
            return diagnostics;
        const auto syntax = TokenizeSource(SourceLanguage::HoroScript, text);
        if (syntax.fallback != SourceSyntaxFallback::None)
            return diagnostics;
        constexpr std::string_view receiver = Extensions::LuaModuleHostReceiver;
        for (auto start = text.find(receiver); start != text.npos && diagnostics.items.size() < MaximumImportDiagnostics;
             start = text.find(receiver, start + receiver.size())) {
            if (!HostReceiverAt(text, syntax, start))
                continue;
            if (auto diagnostic = DiagnoseImport(text, start, imports))
                diagnostics.items.push_back(std::move(*diagnostic));
        }
        return diagnostics;
    }
}  // namespace Horo::Editor::Language
