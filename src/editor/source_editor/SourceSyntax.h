/** @file SourceSyntax.h
 * @brief Bounded, backend-neutral lexical decorations for source editor adapters.
 */
#pragma once

#include "Horo/Editor/SourceEditingCommands.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    /** @brief Lexical families admitted by the initial embedded source editor. */
    enum class SourceLanguage : std::uint8_t {
        PlainText,
        Cpp,
        HoroScript,
        Shader,
        Json,
        Toml,
        Yaml,
        Ini,
        Xml,
        CMake
    };
    /** @brief Semantic presentation roles; no editor-library palette identities escape the adapter. */
    enum class SourceSyntaxRole : std::uint8_t {
        Text,
        Keyword,
        Type,
        String,
        Number,
        Comment,
        Directive,
        Property,
        Count
    };

    /** @brief Half-open UTF-8 byte range; gaps retain the ordinary source-text role. */
    struct SourceSyntaxSpan {
        std::size_t begin{};
        std::size_t end{};
        SourceSyntaxRole role{SourceSyntaxRole::Text};
    };
    /** @brief Explicit readability fallback reasons, independent of document validity/save admission. */
    enum class SourceSyntaxFallback : std::uint8_t {
        None,
        UnknownLanguage,
        Malformed,
        BudgetExceeded
    };

    /** @brief Immutable lexical result tied by the adapter to its current document publication. */
    struct SourceSyntax {
        SourceLanguage language{SourceLanguage::PlainText};
        SourceSyntaxFallback fallback{SourceSyntaxFallback::None};
        std::vector<SourceSyntaxSpan> spans;
        /** @brief Finds the role at a byte offset in logarithmic time. @param offset UTF-8 byte offset. @return Text for gaps. */
        [[nodiscard]] SourceSyntaxRole RoleAt(std::size_t offset) const noexcept;
    };

    /** @brief Work/storage admission limits; exceeded limits preserve readable plain text. */
    struct SourceSyntaxLimits {
        std::size_t maximumBytes{2 * 1024 * 1024};
        std::size_t maximumLineBytes{65'536};
        std::size_t maximumSpans{65'536};
    };

    /** @brief Classifies an admitted source identity without filesystem access.
     * @param path UTF-8 source identity. @return Lexical family, or plain text for unknown formats.
     */
    [[nodiscard]] SourceLanguage ClassifySourceLanguage(std::string_view path) noexcept;
    /** @brief Builds sorted nonoverlapping lexical decorations with bounded input and result storage.
     * @param language Admitted lexical family. @param text Borrowed document bytes.
     * @param limits Work/storage admission. @return Decorations, or an empty plain-text fallback for malformed/oversized input.
     * @note Call on document publication changes, never for unchanged text during drawing. No I/O, jobs, or document mutations occur.
     */
    [[nodiscard]] SourceSyntax TokenizeSource(SourceLanguage language, std::string_view text, SourceSyntaxLimits limits = {});
    /** @brief Plans a newline using the admitted lexical family, retaining canonical byte/history ownership.
     * @param source Authoritative snapshot. @param view Exact current selection.
     * @return Canonical edit plan; unknown or malformed languages retain ordinary indentation.
     */
    [[nodiscard]] Result<SourceEditingPlan> PlanSourceLanguageNewline(const SourceDocumentSnapshot &source, const SourceEditingView &view);
}  // namespace Horo::Editor
