#include "Horo/Hosts/ErrorTranslation.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <utility>

namespace Horo::Hosts {
    namespace {
        /** @brief Checks the stable presentation category independently of error message text. */
        [[nodiscard]] bool ValidCategory(const ExitCategory category) noexcept {
            using enum ExitCategory;
            switch (category) {
                case Usage:
                case Validation:
                case Capability:
                case Operation:
                case Permission:
                case Cancelled:
                case Timeout:
                case Invariant:
                    return true;
            }
            return false;
        }

        /** @brief Bounds each disclosed field and rejects malformed scalar text before JSON serialization. */
        [[nodiscard]] bool ValidText(const std::string_view text) noexcept {
            return text.size() <= 4096 && IsValidUtf8ScalarSequence(text);
        }

        /** @brief Converts only admitted error severities to the canonical wire vocabulary. */
        [[nodiscard]] std::string_view SeverityName(const ErrorSeverity severity) noexcept {
            using enum ErrorSeverity;
            switch (severity) {
                case Info:
                    return "info";
                case Warning:
                    return "warning";
                case Error:
                    return "error";
                case Critical:
                    return "fatal";
            }
            return {};
        }

        /** @brief Validates and projects one diagnostic in its containing error's declared domain. */
        [[nodiscard]] std::optional<Diagnostic> ProjectDiagnostic(const Diagnostic &diagnostic, const ErrorDomainId &domain,
                                                                  const ErrorCodeRegistry &registry, const ErrorDetail detail) {
            using enum DiagnosticSeverity;
            if (diagnostic.severity > Fatal || !ValidText(diagnostic.code.Value()))
                return std::nullopt;
            const auto *descriptor = registry.Resolve(domain, ErrorCode{diagnostic.code.Value()});
            if (descriptor == nullptr || !ValidText(descriptor->summary))
                return std::nullopt;
            if (const auto maximumSeverity = *DiagnosticSeverityForError(descriptor->defaultSeverity);
                diagnostic.severity > maximumSeverity || (maximumSeverity == Fatal && diagnostic.severity != Fatal))
                return std::nullopt;
            const bool trusted = detail == ErrorDetail::TrustedLocal;
            if (trusted && (!ValidText(diagnostic.message) || !ValidText(diagnostic.location.source) || !ValidText(diagnostic.path)))
                return std::nullopt;
            return Diagnostic{diagnostic.code, diagnostic.severity, trusted ? diagnostic.message : std::string{descriptor->summary},
                              trusted ? diagnostic.location : SourceLocation{}, trusted ? diagnostic.path : ""};
        }

        /** @brief Encodes an admitted diagnostic with typed location fields and textual severity. */
        [[nodiscard]] nlohmann::json DiagnosticJson(const Diagnostic &diagnostic) {
            constexpr std::array<std::string_view, 4> names{"note", "warning", "error", "fatal"};
            return {{"code", diagnostic.code.Value()},
                    {"severity", names[static_cast<unsigned>(diagnostic.severity)]},
                    {"message", diagnostic.message},
                    {"location",
                     {{"source", diagnostic.location.source}, {"line", diagnostic.location.line}, {"column", diagnostic.location.column}}},
                    {"path", diagnostic.path}};
        }

        /** @brief Validates every cause against the retained registry before constructing an owned safe chain. */
        [[nodiscard]] std::optional<Error> ProjectError(const Error &error, const ErrorCodeRegistry &registry, const ErrorDetail detail,
                                                        const std::size_t depth) {
            const auto *descriptor = registry.Resolve(error);
            if (depth == 16 || descriptor == nullptr || error.severity > descriptor->defaultSeverity ||
                (descriptor->defaultSeverity == ErrorSeverity::Critical && error.severity != ErrorSeverity::Critical) ||
                SeverityName(error.severity).empty() || error.diagnostics.size() > 64 || !ValidText(descriptor->summary) ||
                !ValidText(descriptor->remediationHint))
                return std::nullopt;
            const bool trusted = detail == ErrorDetail::TrustedLocal;
            if (trusted && !ValidText(error.message))
                return std::nullopt;
            Error projected = MakeError(*descriptor, trusted && !error.message.empty() ? error.message : std::string{descriptor->summary});
            projected.severity = error.severity;
            for (const Diagnostic &diagnostic : error.diagnostics) {
                auto projectedDiagnostic = ProjectDiagnostic(diagnostic, error.domain, registry, detail);
                if (!projectedDiagnostic)
                    return std::nullopt;
                projected.diagnostics.push_back(std::move(*projectedDiagnostic));
            }
            if (const Error *cause = error.cause.Get()) {
                auto projectedCause = ProjectError(*cause, registry, detail, depth + 1);
                if (!projectedCause)
                    return std::nullopt;
                projected = WithCause(std::move(projected), std::move(*projectedCause));
            }
            return projected;
        }

        /** @brief Serializes only the already admitted owned projection, preserving cause nesting and diagnostic order. */
        [[nodiscard]] nlohmann::json ErrorJson(const Error &error) {
            nlohmann::json diagnostics = nlohmann::json::array();
            for (const Diagnostic &diagnostic : error.diagnostics)
                diagnostics.push_back(DiagnosticJson(diagnostic));
            return {{"domain", error.domain.Value()},
                    {"code", error.code.Value()},
                    {"severity", SeverityName(error.severity)},
                    {"message", error.message},
                    {"diagnostics", std::move(diagnostics)},
                    {"metadata", nlohmann::json::object()},
                    {"cause", error.cause ? ErrorJson(*error.cause.Get()) : nlohmann::json(nullptr)}};
        }
    }  // namespace

    TranslatedError::TranslatedError(Error error, const ExitCategory category, std::string summary, std::string remediation,
                                     const bool retryable, const bool userActionable, std::string json)
        : error_(std::move(error)), category_(category), summary_(std::move(summary)), remediation_(std::move(remediation)),
          retryable_(retryable), userActionable_(userActionable), json_(std::move(json)) {}

    ErrorTranslator::ErrorTranslator(ErrorCodeRegistry registry, std::vector<ErrorMapping> mappings)
        : registry_(std::move(registry)), mappings_(std::move(mappings)) {}

    /** @copydoc ErrorTranslator::Create */
    std::optional<ErrorTranslator> ErrorTranslator::Create(ErrorCodeRegistry registry, const std::span<const ErrorMapping> mappings) {
        for (std::size_t index = 0; index < mappings.size(); ++index) {
            const auto &mapping = mappings[index];
            if (!ValidCategory(mapping.category) || registry.Resolve(mapping.domain, mapping.code) == nullptr)
                return std::nullopt;
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (mappings[previous].domain.Value() == mapping.domain.Value() && mappings[previous].code.Value() == mapping.code.Value())
                    return std::nullopt;
            }
        }
        return ErrorTranslator{std::move(registry), {mappings.begin(), mappings.end()}};
    }

    /** @copydoc ErrorTranslator::Translate */
    std::optional<TranslatedError> ErrorTranslator::Translate(const Error &error, const ErrorDetail detail) const {
        if (detail != ErrorDetail::Public && detail != ErrorDetail::TrustedLocal)
            return std::nullopt;
        const auto mapping = std::ranges::find_if(mappings_, [&error](const ErrorMapping &candidate) {
            return candidate.domain.Value() == error.domain.Value() && candidate.code.Value() == error.code.Value();
        });
        if (mapping == mappings_.end())
            return std::nullopt;
        auto projected = ProjectError(error, registry_, detail, 0);
        if (!projected)
            return std::nullopt;
        const auto &descriptor = *registry_.Resolve(error);
        auto json = ErrorJson(*projected).dump();
        return TranslatedError{std::move(*projected),
                               mapping->category,
                               std::string{descriptor.summary},
                               std::string{descriptor.remediationHint},
                               descriptor.retryable,
                               descriptor.userActionable,
                               std::move(json)};
    }

    /** @copydoc TranslateGuiError */
    GuiErrorPresentation TranslateGuiError(TranslatedError error, const GuiErrorSurface surface) {
        const auto resolved = error.Failure().severity == ErrorSeverity::Critical ? GuiErrorSurface::FatalDialog : surface;
        return {resolved, std::move(error)};
    }

    /** @copydoc TranslateCliError */
    CliErrorPresentation TranslateCliError(const TranslatedError &error) {
        const int exitCode = static_cast<int>(error.Category());
        const nlohmann::json envelope{{"schemaVersion", 1}, {"exitCode", exitCode}, {"error", nlohmann::json::parse(error.Json())}};
        return {exitCode, error.Failure().domain.Value() + "/" + error.Failure().code.Value() + ": " + error.Failure().message + "\n",
                envelope.dump()};
    }

    /** @copydoc TranslateMcpError */
    std::string TranslateMcpError(const TranslatedError &error) {
        return nlohmann::json{{"code", -32000}, {"message", error.Summary()}, {"data", nlohmann::json::parse(error.Json())}}.dump();
    }
}  // namespace Horo::Hosts
