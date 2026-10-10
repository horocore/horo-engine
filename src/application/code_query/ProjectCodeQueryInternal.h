#pragma once

#include "Horo/Application/ProjectCodeQuery.h"

namespace Horo::Application::CodeQueryDetail {
    /** @brief Per-invocation stop view, retaining no application state or job. */
    struct Scope final {
        const CodeQueryContext &context;
        std::chrono::steady_clock::time_point deadline;
        /** @brief Checks the complete cooperative cancellation/deadline view. */
        [[nodiscard]] Result<void> Check() const;
    };

    /** @brief Validates canonical portable UTF-8 path spelling without filesystem I/O. */
    [[nodiscard]] bool ValidPath(std::string_view path, bool allowRoot = false);
    /** @brief Checks finite host policy against the public hard limits. */
    [[nodiscard]] bool ValidLimits(const CodeQueryLimits &limits);
    /** @brief Rejects control/binary bytes and noncanonical UTF-8. */
    [[nodiscard]] Result<void> ValidateText(std::string_view text, const Scope &scope);
    /** @brief Checks requested revision and continuation offset against an exact captured observation. */
    [[nodiscard]] Result<void> Fence(const CodeQueryRequest &request, std::string_view revision, std::size_t size);
    /** @brief Builds a bounded UTF-8 byte page without splitting a scalar. */
    [[nodiscard]] Result<CodeQueryPage> TextPage(const CodeQueryRequest &request, const CodeQueryTextSnapshot &snapshot);
    /** @brief Performs budgeted literal search with no regex/backtracking execution. */
    [[nodiscard]] Result<CodeQueryObservation> Search(const CodeQueryRequest &request, const CodeQueryTextSnapshot &snapshot,
                                                      const CodeQueryLimits &limits, const Scope &scope);
    /** @brief Validates provider provenance/rows and projects a bounded continuation page. */
    [[nodiscard]] Result<CodeQueryPage> ObservationPage(const CodeQueryRequest &request, CodeQueryObservation observation,
                                                        const CodeQueryContext &context, const CodeQueryLimits &limits, const Scope &scope);
}  // namespace Horo::Application::CodeQueryDetail
