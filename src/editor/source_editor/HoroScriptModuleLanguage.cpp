#include "HoroScriptModuleLanguage.h"

#include "ScriptModuleHostServices.h"
#include "SourceSyntax.h"

#include <algorithm>

namespace Horo::Editor::Language {
    namespace {
        constexpr std::size_t MaximumExpressionBytes = 4096;
        constexpr std::size_t MaximumChoices = 128;

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
            if (end == text.npos || alias.empty() || alias.find_first_of("\\\r\n") != alias.npos)
                return std::nullopt;
            text.remove_prefix(end + 1);
            return alias;
        }

        /** @brief Reads the exact import call without evaluating the expression. */
        std::optional<std::string_view> ReadAlias(std::string_view &text) {
            if (!Take(text, "horo") || !Take(text, ".") || !Take(text, "import") || !Take(text, "("))
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

        /** @brief A direct, static import expression ending at the requested member. */
        struct ImportMember final {
            std::string_view alias;
            std::string path;
            LanguageByteSpan member;
        };

        /** @brief Refuses textual matches in comments, strings, or qualified receivers. */
        bool ImportHead(const std::string_view text, const SourceSyntax &syntax, const std::size_t start) {
            if (syntax.RoleAt(start) != SourceSyntaxRole::Text)
                return false;
            const auto previous = text.substr(0, start).find_last_not_of(" \t\r\n\v\f");
            if (previous == text.npos)
                return true;
            return text[previous] != '.' && text[previous] != ':' && (previous + 1 != start || !Identifier(text[previous]));
        }

        /** @brief Searches a finite expression window and validates all consumed delimiters. */
        std::optional<ImportMember> FindImportMember(const std::string_view text, const SourceSyntax &syntax, const std::size_t offset) {
            const auto member = MemberSpan(text, offset);
            const auto left = text.substr(0, member.start);
            const auto lower = member.start > MaximumExpressionBytes ? member.start - MaximumExpressionBytes : 0;
            auto start = left.rfind("horo");
            while (start != text.npos && start >= lower) {
                if (ImportHead(text, syntax, start)) {
                    auto expression = left.substr(start);
                    const auto alias = ReadAlias(expression);
                    if (alias) {
                        if (auto path = ReadNamespace(expression))
                            return ImportMember{*alias, std::move(*path), member};
                    }
                }
                if (start == 0)
                    break;
                start = left.rfind("horo", start - 1);
            }
            return std::nullopt;
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

        /** @brief Appends one valid identifier completion, retaining the original replacement range. */
        void AddChoice(LanguageCompletions &completion, const ImportMember &member, const std::string_view prefix,
                       const std::string_view name, std::string detail = {}) {
            if (name.empty() || !name.starts_with(prefix) || !std::ranges::all_of(name, Identifier))
                return;
            if (completion.items.size() == MaximumChoices) {
                completion.incomplete = true;
                return;
            }
            completion.items.push_back({.label = std::string{name},
                                        .detail = std::move(detail),
                                        .insertText = std::string{name},
                                        .edits = {{member.member, std::string{name}}},
                                        .primaryEdit = member.member});
        }

        /** @brief Resolves the next namespace segment or actual published members. */
        LanguageCompletions Complete(const Extensions::ScriptExportDescriptor &descriptor, const ImportMember &member,
                                     const std::string_view prefix) {
            LanguageCompletions completion;
            const std::string_view nameSpace = descriptor.nameSpace;
            if (nameSpace == member.path) {
                for (const auto &function : descriptor.functions)
                    AddChoice(completion, member, prefix, function.id, FormatSignature(function));
                for (const auto &constant : descriptor.constants)
                    AddChoice(completion, member, prefix, constant.id, constant.documentation.summary);
                std::ranges::sort(completion.items, {}, &LanguageCompletionItem::label);
            } else if (member.path.empty()) {
                AddChoice(completion, member, prefix, nameSpace.substr(0, nameSpace.find('.')));
            } else if (nameSpace.starts_with(member.path) && nameSpace[member.path.size()] == '.') {
                const auto suffix = nameSpace.substr(member.path.size() + 1);
                AddChoice(completion, member, prefix, suffix.substr(0, suffix.find('.')));
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

        /** @brief Resolves a lexical import expression and queries its admitted descriptor. */
        LanguagePayload ProjectImport(const std::string_view text, const SourceLanguageRequestKind kind, const std::size_t offset,
                                      const std::span<const Extensions::ScriptModuleApiMetadata> imports) {
            const auto syntax = TokenizeSource(SourceLanguage::HoroScript, text);
            if (syntax.fallback != SourceSyntaxFallback::None)
                return std::monostate{};
            const auto member = FindImportMember(text, syntax, offset);
            if (!member)
                return std::monostate{};
            const auto imported = std::ranges::find(imports, member->alias, &Extensions::ScriptModuleApiMetadata::alias);
            if (imported == imports.end() || !imported->descriptor)
                return std::monostate{};
            if (kind == SourceLanguageRequestKind::Completion)
                return Complete(*imported->descriptor, *member, text.substr(member->member.start, offset - member->member.start));
            return DescribeMember(*imported->descriptor, *member,
                                  text.substr(member->member.start, member->member.end - member->member.start));
        }
    }  // namespace

    /** @copydoc QueryHoroScriptImports */
    Result<LanguageResult> QueryHoroScriptImports(const SourceDocumentSnapshot &source, const SourceLanguageRequestKind kind,
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
        return Output::Success({{kind, source}, ProjectImport(text, kind, offset, imports)});
    }
}  // namespace Horo::Editor::Language
