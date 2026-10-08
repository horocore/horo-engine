#pragma once

/**
 * @file HostObservability.h
 * @brief Shared graphical and headless host observability composition.
 */

#include "Horo/Foundation/Diagnostics/DiagnosticBundle.h"
#include "Horo/Foundation/Logging/Logger.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Application {
    /** @brief Privacy-reviewed identity attached to startup records and support bundles. */
    struct HostObservabilityIdentity {
        std::string processRole;        /**< Stable role such as editor, cli, game, or packager. */
        std::string engineVersion;      /**< Horo Engine release version. */
        std::string buildConfiguration; /**< Debug, Release, or another build configuration. */
        std::string sourceRevision;     /**< Optional source revision without a repository path. */
        std::string rendererBackend;    /**< Optional backend identity for render-capable hosts. */
        std::string deviceName;         /**< Optional non-sensitive graphics device identity. */
        std::string projectId;          /**< Optional stable project identity, never a project path. */
        std::string projectVersion;     /**< Optional project schema/release version. */
    };

    /** @brief One optional, caller-approved JSON or JSONL diagnostic file with a fixed bundle namespace. */
    struct HostDiagnosticFile {
        std::filesystem::path sourcePath;  /**< Host-owned local JSON or JSONL diagnostic source. */
        std::filesystem::path archivePath; /**< Relative .json/.jsonl path under crash, configuration, packages, or metadata. */
    };

    /** @brief Inputs for a user-initiated support bundle generated from one host session. */
    struct HostDiagnosticBundleRequest {
        std::filesystem::path outputPath;                          /**< Absolute destination for a new ZIP file. */
        std::optional<HostDiagnosticFile> crashMetadata;           /**< Optional crash reporter output. */
        std::optional<HostDiagnosticFile> configuration;           /**< Optional redacted effective configuration. */
        std::optional<HostDiagnosticFile> packageSummary;          /**< Optional package/plugin inventory summary. */
        std::vector<std::pair<std::string, std::string>> metadata; /**< Additional caller-vetted metadata. */
        std::uintmax_t maxInputBytes{64U * 1024U * 1024U};         /**< Aggregate uncompressed input bound. */
    };

    /** @brief Capture-free host contribution evaluated only during explicit export after log flush.
     * @details Inputs are the session's retained JSONL files, never discovered subsystem state. Providers must bound
     * row/byte work and return a canonical privacy-reviewed metadata pair. They own no independent event store.
     */
    using HostDiagnosticSummaryProvider = Result<std::pair<std::string, std::string>> (*)(std::span<const std::filesystem::path>);

    /** @brief Typed process-level configuration shared by graphical and headless hosts. */
    struct HostObservabilityConfiguration {
        Log::LoggerConfiguration logging;                     /**< Queue, filtering, local persistence, and optional sink policy. */
        HostObservabilityIdentity identity;                   /**< Safe process/build/project identity. */
        std::vector<HostDiagnosticSummaryProvider> summaries; /**< At most eight explicit capture-free summary contributions. */
    };

    /** @brief RAII owner of the process observability lifecycle and support-bundle composition. */
    class HostObservabilitySession final {
    private:
        struct ConstructionToken final {};

    public:
        HostObservabilitySession(const HostObservabilitySession &) = delete;
        HostObservabilitySession &operator=(const HostObservabilitySession &) = delete;
        HostObservabilitySession(HostObservabilitySession &&) = delete;
        HostObservabilitySession &operator=(HostObservabilitySession &&) = delete;

        /**
         * @brief Starts the process logger and emits the host identity snapshot.
         * @param configuration Validated host-owned composition.
         * @return Session owner, or null when local persistence cannot be initialized.
         */
        [[nodiscard]] static std::unique_ptr<HostObservabilitySession> Start(HostObservabilityConfiguration configuration);

        /** @brief Performs bounded reverse shutdown when still active. */
        ~HostObservabilitySession();

        /**
         * @brief Flushes current diagnostics and creates an allowlisted portable bundle.
         * @param request User-approved output and optional diagnostic summaries.
         * @return Committed bundle summary or a typed diagnostic-bundle error.
         */
        [[nodiscard]] Result<Diagnostics::DiagnosticBundleSummary> GenerateDiagnosticBundle(
            const HostDiagnosticBundleRequest &request) const;

        /** @brief Stops admission and drains sinks; repeated calls are harmless. */
        void Shutdown() noexcept;

        /** @brief Returns the resolved absolute directory containing this session's local diagnostics. */
        [[nodiscard]] const std::filesystem::path &LogDirectory() const noexcept;

        /**
         * @brief Constructs a session through the private lifecycle token used by Start.
         * @param token Internal construction capability; callers cannot create one outside this class.
         * @param configuration Validated host-owned composition.
         */
        explicit HostObservabilitySession(ConstructionToken token, HostObservabilityConfiguration configuration);

    private:
        HostObservabilityConfiguration configuration_;
        bool active_{true};
    };
}  // namespace Horo::Application
