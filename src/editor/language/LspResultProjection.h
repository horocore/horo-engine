#pragma once

/** @file LspResultProjection.h @brief Typed, revision-bound results produced by the language adapter. */

#include "Horo/Editor/SourceEditingCommands.h"
#include "Horo/Editor/SourceLanguageSession.h"

#include <variant>
#include <vector>

namespace Horo::Editor::Language {
    struct LspIncoming;
    struct LspMessage;

    inline constexpr std::string_view PublishDiagnosticsMethod = "textDocument/publishDiagnostics";

    /** @brief Validated routing key for a versioned diagnostic notification. */
    struct LspDiagnosticTarget final {
        SourceDocumentId source;
        std::int32_t wireVersion{};
    };

    /** @brief Half-open byte span in the retained source snapshot. Empty spans are valid. */
    struct LanguageByteSpan final {
        std::size_t start{};
        std::size_t end{};
        auto operator<=>(const LanguageByteSpan &) const = default;
    };

    /** @brief Source coordinate preserved for a navigation target whose text is not yet loaded. */
    struct LanguagePosition final {
        std::uint32_t line{};
        std::uint32_t utf16Column{};
        auto operator<=>(const LanguagePosition &) const = default;
    };

    /** @brief One validated project destination, still requiring native validation before opening. */
    struct LanguageLocation final {
        SourceDocumentId source;
        LanguagePosition start;
        LanguagePosition end;
    };

    /** @brief One explicit navigation to a validated, current source selection. */
    struct LanguageSourceFocus final {
        SourceDocumentSnapshot source;
        LanguageByteSpan selection;
    };

    /** @brief Resolves a provider destination only against the matching current source publication. */
    [[nodiscard]] std::optional<LanguageByteSpan> ResolveLanguageLocation(const SourceDocumentSnapshot &source,
                                                                          const LanguageLocation &location);

    /** @brief Owned proposed text edit, interpreted only against the enclosing source lease. */
    struct LanguageTextEdit final {
        LanguageByteSpan span;
        std::string replacement;
        bool operator==(const LanguageTextEdit &) const = default;
    };

    /** @brief Diagnostic urgency in protocol order; omitted severity is unspecified. */
    enum class LanguageSeverity {
        Unspecified,
        Error,
        Warning,
        Information,
        Hint
    };

    /** @brief A server diagnostic projected onto the exact source bytes. */
    struct LanguageDiagnostic final {
        LanguageByteSpan span;
        LanguageSeverity severity{};
        std::string message;
    };

    /** @brief Complete projected diagnostics for one source revision. */
    struct LanguageDiagnostics final {
        std::vector<LanguageDiagnostic> items;
    };

    /** @brief Inert hover text and its optional source selection. */
    struct LanguageHover final {
        std::string text;
        std::optional<LanguageByteSpan> span;
    };

    /** @brief Completion display metadata and optional validated source replacements. */
    struct LanguageCompletionItem final {
        std::string label;
        std::string detail;
        std::string insertText;
        std::vector<LanguageTextEdit> edits;
        std::optional<LanguageByteSpan> primaryEdit; /**< Identifies the main range among the sorted edits. */
        bool operator==(const LanguageCompletionItem &) const = default;
    };

    /** @brief Plans one atomic and undoable completion, including its additional document edits. */
    [[nodiscard]] Result<SourceEditingPlan> PlanLanguageCompletion(const SourceDocumentSnapshot &source, const SourceEditingView &view,
                                                                   const LanguageCompletionItem &item);

    /** @brief Bounded completion candidates and the peer's completeness indicator. */
    struct LanguageCompletions final {
        std::vector<LanguageCompletionItem> items;
        bool incomplete{};
    };

    /** @brief Project-scoped results for definitions or references. */
    struct LanguageNavigation final {
        std::vector<LanguageLocation> targets;
    };

    /** @brief A named symbol and its selection in the requesting document. */
    struct LanguageSymbol final {
        std::string name;
        LanguageByteSpan selection;
    };

    /** @brief Bounded document symbols flattened in server order. */
    struct LanguageSymbols final {
        std::vector<LanguageSymbol> items;
    };

    /** @brief Display title and optional inert, current-document replacements. */
    struct LanguageCodeAction final {
        std::string title;
        std::vector<LanguageTextEdit> edits;
    };

    /** @brief Code-action proposals available for explicit review. */
    struct LanguageCodeActions final {
        std::vector<LanguageCodeAction> items;
    };

    /** @brief Typed server rejection for a current document request. */
    struct LanguageServerFailure final {
        std::int32_t code{};
        std::string message;
    };

    using LanguagePayload = std::variant<std::monostate, LanguageDiagnostics, LanguageHover, LanguageCompletions, LanguageNavigation,
                                         LanguageSymbols, LanguageCodeActions, LanguageServerFailure>;

    /** @brief Retains provenance for every feature result and any proposed edits. */
    struct LanguageResult final {
        SourceLanguageReply context;
        LanguagePayload payload;
    };

    /** @brief Projects a correlated reply or diagnostic publication without opening files or applying edits.
     * @param incoming Message, exact source snapshot and wire version. @param rootUri Host-owned project URI.
     * @return A mapped value, an empty payload for null, or a refusal for stale or malformed data.
     */
    [[nodiscard]] Result<LanguageResult> ProjectLspReply(const LspIncoming &incoming, std::string_view rootUri);

    /** @brief Returns a project-scoped target only when the peer supplies a usable document version. */
    [[nodiscard]] std::optional<LspDiagnosticTarget> ReadLspDiagnosticTarget(const LspMessage &message, std::string_view rootUri);
}  // namespace Horo::Editor::Language
