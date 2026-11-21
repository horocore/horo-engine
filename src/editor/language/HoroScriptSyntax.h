#pragma once

/** @file HoroScriptSyntax.h @brief Bounded syntax diagnostics for Lua-backed source documents. */

#include "Horo/Foundation/JobSystem.h"
#include "LspResultProjection.h"

namespace Horo::Editor {
    class SourceFileOpenService;
}

namespace Horo::Editor::Language {
    /** @brief Parses Lua text without opening standard libraries, executing the chunk or returning a VM handle. */
    [[nodiscard]] Result<LanguageResult> CheckHoroScriptSyntax(const SourceDocumentSnapshot &source);

    /** @brief Coalesces active-script checks to one worker and publishes only current document results. */
    class HoroScriptSyntaxWorkspace final {
    public:
        HoroScriptSyntaxWorkspace() = default;
        ~HoroScriptSyntaxWorkspace();
        HoroScriptSyntaxWorkspace(const HoroScriptSyntaxWorkspace &) = delete;
        HoroScriptSyntaxWorkspace &operator=(const HoroScriptSyntaxWorkspace &) = delete;

        /** @brief Checks the active source if policy admits it as Horo Script. Empty input retires work without blocking. */
        [[nodiscard]] std::optional<LanguageResult> Poll(const JobSystem &jobs, const SourceFileOpenService &sources,
                                                         const SourceDocumentSnapshot &source);

    private:
        /** @brief Owns the result handoff until the scheduler publishes a terminal state. */
        struct Work final {
            SourceDocumentSnapshot source;
            JobHandle job;
            std::shared_ptr<std::optional<LanguageResult>> result;
        };

        /** @brief Starts a bounded background parse using an immutable snapshot. */
        void Start(const JobSystem &jobs, const SourceDocumentSnapshot &source);
        /** @brief Attempts one retryable admission for a newly selected script snapshot. */
        void Admit(const JobSystem &jobs, const SourceFileOpenService &sources, const SourceDocumentSnapshot &source);

        std::optional<Work> work_;
        SourceDocumentSnapshot attempted_;
        std::chrono::steady_clock::time_point retryAt_{};
    };
}  // namespace Horo::Editor::Language
