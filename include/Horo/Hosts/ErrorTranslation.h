#pragma once

/**
 * @file ErrorTranslation.h
 * @brief Registry-backed reference presentation adapters for application errors.
 */

#include "Horo/Foundation/ErrorCodeRegistry.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::Hosts {
    /** @brief Stable CLI presentation categories, never application failure identities. */
    enum class ExitCategory : std::uint8_t {
        Usage = 2,
        Validation = 3,
        Capability = 4,
        Operation = 5,
        Permission = 6,
        Cancelled = 7,
        Timeout = 8,
        Invariant = 10
    };

    /** @brief Explicit composition-root mapping for one externally exposed identity. */
    struct ErrorMapping {
        ErrorDomainId domain;
        ErrorCode code;
        ExitCategory category{ExitCategory::Operation};
    };

    /** @brief Public output omits operation text and locations; trusted local details retain them. */
    enum class ErrorDetail : std::uint8_t {
        Public,
        TrustedLocal
    };

    /** @brief Immutable owned presentation preserving the original identity and cause chain. */
    class TranslatedError final {
    public:
        /** @brief Returns the owned safe application error. @return Immutable typed failure detail. */
        [[nodiscard]] const Error &Failure() const noexcept {
            return error_;
        }

        /** @brief Returns the stable presentation category. @return CLI category, independent of business identity. */
        [[nodiscard]] ExitCategory Category() const noexcept {
            return category_;
        }

        /** @brief Returns the registered summary. @return Owned public fallback copy. */
        [[nodiscard]] const std::string &Summary() const noexcept {
            return summary_;
        }

        /** @brief Returns registered remediation. @return Owned public guidance. */
        [[nodiscard]] const std::string &Remediation() const noexcept {
            return remediation_;
        }

        /** @brief Reports the descriptor retry policy. @return Whether retry is declared valid. */
        [[nodiscard]] bool Retryable() const noexcept {
            return retryable_;
        }

        /** @brief Reports the descriptor actionability. @return Whether user action can resolve the failure. */
        [[nodiscard]] bool UserActionable() const noexcept {
            return userActionable_;
        }

        /** @brief Returns the canonical payload. @return JSON derived from the immutable owned failure. */
        [[nodiscard]] const std::string &Json() const noexcept {
            return json_;
        }

    private:
        TranslatedError(Error error, ExitCategory category, std::string summary, std::string remediation, bool retryable,
                        bool userActionable, std::string json);
        Error error_;
        ExitCategory category_;
        std::string summary_;
        std::string remediation_;
        bool retryable_{};
        bool userActionable_{};
        std::string json_;
        friend class ErrorTranslator;
    };

    /** @brief Immutable tooling-time translator owning its registry snapshot and exact mapping table. */
    class ErrorTranslator final {
    public:
        /**
         * @brief Validates and copies an explicit external translation scope.
         * @param registry Active module registry snapshot.
         * @param mappings Exact identities and stable presentation categories exposed by this host.
         * @return Translator, or no value for duplicate, undeclared or invalid mappings.
         * @note Missing mappings fail translation; no message parsing or domain-prefix inference occurs.
         */
        [[nodiscard]] static std::optional<ErrorTranslator> Create(ErrorCodeRegistry registry, std::span<const ErrorMapping> mappings);

        /**
         * @brief Projects one application error with a bounded, explicitly trusted disclosure policy.
         * @param error Original application outcome; never mutated.
         * @param detail Whether local operation messages and source locations may be disclosed.
         * @return Owned presentation, or no value for an unmapped identity, undeclared cause or diagnostic,
         *         invalid severity, invalid UTF-8, more than 16 nodes, 64 diagnostics per node,
         *         or any admitted text field exceeding 4096 bytes.
         * @note All cause severities obey their descriptors. Critical cannot be downgraded;
         *       other severities may be lowered but never raised above the descriptor default.
         *       Diagnostic identities resolve in their containing error's domain.
         */
        [[nodiscard]] std::optional<TranslatedError> Translate(const Error &error, ErrorDetail detail = ErrorDetail::Public) const;

    private:
        ErrorTranslator(ErrorCodeRegistry registry, std::vector<ErrorMapping> mappings);
        ErrorCodeRegistry registry_;
        std::vector<ErrorMapping> mappings_;
    };

    /** @brief GUI workflow context supplied by the owning presentation boundary. */
    enum class GuiErrorSurface : std::uint8_t {
        Inline,
        Notification,
        Workflow,
        FatalDialog
    };

    /** @brief Reference GUI model; localized text may replace summaries without changing details. */
    struct GuiErrorPresentation {
        GuiErrorSurface surface;
        TranslatedError detail;
    };

    /** @brief Reference CLI output; the process host writes stderrText to stderr and returns exitCode. */
    struct CliErrorPresentation {
        int exitCode;
        std::string stderrText;
        std::string json; /**< Version-one envelope consumed by Python tooling. */
    };

    /**
     * @brief Maps an already validated error to a GUI model, forcing critical failures to a fatal dialog.
     * @param error Validated shared presentation.
     * @param surface Requested workflow surface.
     * @return Owned GUI model with the unchanged typed detail.
     */
    [[nodiscard]] GuiErrorPresentation TranslateGuiError(TranslatedError error, GuiErrorSurface surface);

    /**
     * @brief Creates stable exit status, stderr text and versioned machine output.
     * @param error Validated shared presentation.
     * @return CLI envelope retaining the canonical error payload.
     */
    [[nodiscard]] CliErrorPresentation TranslateCliError(const TranslatedError &error);

    /**
     * @brief Wraps a valid application failure in a JSON-RPC engine-error object.
     * @param error Validated shared presentation; protocol parse/admission errors use their own envelope.
     * @return JSON error object with protocol code -32000 and the canonical payload in data.
     */
    [[nodiscard]] std::string TranslateMcpError(const TranslatedError &error);
}  // namespace Horo::Hosts
