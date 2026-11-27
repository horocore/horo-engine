#pragma once

/** @file HoroScriptSyntax.h @brief Bounded syntax diagnostics for Lua-backed source documents. */

#include "Horo/Foundation/JobSystem.h"
#include "LspResultProjection.h"

#include <functional>

namespace Horo::Extensions {
    struct ScriptModuleApiMetadata;
}

namespace Horo::Editor {
    class SourceFileOpenService;
}

namespace Horo::Editor::Language {
    /** @brief Parses Lua text without opening standard libraries, executing the chunk or returning a VM handle. */
    [[nodiscard]] Result<LanguageResult> CheckHoroScriptSyntax(const SourceDocumentSnapshot &source);

    /** @brief Checks bounded detached source bytes without opening or mutating a document.
     * @param source Proposed UTF-8 script, optionally including a BOM.
     * @return Syntax diagnostics in the supplied byte coordinates, or a typed input/budget failure.
     */
    [[nodiscard]] Result<LanguageDiagnostics> CheckHoroScriptSyntaxText(std::string_view source);

    /** @brief Coalesces active-script checks to one worker and publishes only current document results. */
    class HoroScriptSyntaxWorkspace final {
    public:
        /** @brief Captures inert metadata on the workspace owner before submitting the parse job. */
        using CaptureImports = std::function<std::vector<Extensions::ScriptModuleApiMetadata>(const SourceDocumentSnapshot &)>;

        HoroScriptSyntaxWorkspace() = default;
        ~HoroScriptSyntaxWorkspace();
        HoroScriptSyntaxWorkspace(const HoroScriptSyntaxWorkspace &) = delete;
        HoroScriptSyntaxWorkspace &operator=(const HoroScriptSyntaxWorkspace &) = delete;

        /** @brief Checks an admitted active script against the current schema revision.
         * @param jobs Background job owner. @param sources Project source policy.
         * @param source Current source lease, or an empty lease to retire work.
         * @param apiRevision Schema publication identity. @param captureImports Owner-lane metadata query invoked on admission.
         * @return A completed publication only while both source and schema match.
         */
        [[nodiscard]] std::optional<LanguageResult> Poll(const JobSystem &jobs, const SourceFileOpenService &sources,
                                                         const SourceDocumentSnapshot &source, std::uint64_t apiRevision = 0,
                                                         const CaptureImports &captureImports = {});

    private:
        /** @brief Owns the result handoff until the scheduler publishes a terminal state. */
        struct Work final {
            SourceDocumentSnapshot source;
            std::uint64_t apiRevision{};
            JobHandle job;
            std::shared_ptr<std::optional<LanguageResult>> result;

            /** @brief Tests both identities before any worker result can be observed. */
            [[nodiscard]] bool Matches(const SourceDocumentSnapshot &current, std::uint64_t revision) const noexcept;
        };

        /** @brief Starts a bounded background parse using an immutable snapshot. */
        void Start(const JobSystem &jobs, const SourceDocumentSnapshot &source, std::vector<Extensions::ScriptModuleApiMetadata> imports);
        /** @brief Attempts one retryable admission for a newly selected script snapshot. */
        void Admit(const JobSystem &jobs, const SourceFileOpenService &sources, const SourceDocumentSnapshot &source,
                   const CaptureImports &captureImports);

        std::optional<Work> work_;
        SourceDocumentSnapshot attempted_;
        std::uint64_t apiRevision_{};
        std::chrono::steady_clock::time_point retryAt_{};
    };
}  // namespace Horo::Editor::Language
