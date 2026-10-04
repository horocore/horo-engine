#include "Horo/Application/HostObservability.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <format>
#include <string_view>
#include <system_error>
#include <thread>

namespace Horo::Application {
    namespace {
        [[nodiscard]] std::atomic<bool> &HostSessionActive() noexcept {
            static std::atomic<bool> active{};
            return active;
        }

        /** @brief Resolves the one legacy home-relative form accepted by logger composition. */
        [[nodiscard]] std::filesystem::path ResolveLogDirectory(const std::filesystem::path &path) {
            const std::string text = path.string();
            if (text != "~" && !text.starts_with("~/") && !text.starts_with("~\\")) {
                std::error_code error;
                const std::filesystem::path absolute = std::filesystem::absolute(path, error);
                return error ? std::filesystem::path{} : absolute.lexically_normal();
            }
#if defined(_WIN32)
            const char *home = std::getenv("USERPROFILE");
#else
            const char *home = std::getenv("HOME");
#endif
            if (home == nullptr || *home == '\0')
                return {};
            if (text.size() == 1)
                return std::filesystem::path{home}.lexically_normal();
            return (std::filesystem::path{home} / text.substr(2)).lexically_normal();
        }

        [[nodiscard]] std::string OperatingSystemName() {
#if defined(_WIN32)
            return "windows";
#elif defined(__APPLE__)
            return "macos";
#elif defined(__linux__)
            return "linux";
#else
            return "unknown";
#endif
        }

        [[nodiscard]] std::string ArchitectureName() {
#if defined(__aarch64__) || defined(_M_ARM64)
            return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
            return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
            return "x86";
#else
            return "unknown";
#endif
        }

        void AppendMetadata(std::vector<std::pair<std::string, std::string>> &metadata, const std::string_view key,
                            const std::string &value) {
            if (!value.empty())
                metadata.emplace_back(key, value);
        }

        /** @brief Adds the configured host identity to the bundle metadata. */
        void AppendHostIdentity(std::vector<std::pair<std::string, std::string>> &metadata,
                                const HostObservabilityConfiguration &configuration) {
            const HostObservabilityIdentity &identity = configuration.identity;
            AppendMetadata(metadata, "application.name", configuration.logging.hostName);
            AppendMetadata(metadata, "application.version", configuration.logging.hostVersion);
            AppendMetadata(metadata, "process.role", identity.processRole);
            AppendMetadata(metadata, "engine.version", identity.engineVersion);
            AppendMetadata(metadata, "build.configuration", identity.buildConfiguration);
            AppendMetadata(metadata, "source.revision", identity.sourceRevision);
            AppendMetadata(metadata, "os.name", OperatingSystemName());
            AppendMetadata(metadata, "cpu.architecture", ArchitectureName());
            AppendMetadata(metadata, "renderer.backend", identity.rendererBackend);
            AppendMetadata(metadata, "device.name", identity.deviceName);
            AppendMetadata(metadata, "project.id", identity.projectId);
            AppendMetadata(metadata, "project.version", identity.projectVersion);
        }

        /** @brief Appends bounded host-registered summaries from retained log entries. */
        [[nodiscard]] Result<void> AppendConfiguredSummaries(Diagnostics::DiagnosticBundleRequest &bundle,
                                                             const std::vector<HostDiagnosticSummaryProvider> &providers) {
            std::vector<std::filesystem::path> retainedLogs;
            for (const auto &entry : bundle.entries) {
                if (entry.archivePath.parent_path() == "logs")
                    retainedLogs.push_back(entry.sourcePath);
            }
            for (const auto provider : providers) {
                auto summary = provider(retainedLogs);
                if (summary.HasError())
                    return Result<void>::Failure(summary.ErrorValue());
                bundle.metadata.push_back(std::move(summary).Value());
            }
            return Result<void>::Success();
        }

        void AppendOptionalEntry(std::vector<Diagnostics::DiagnosticBundleEntry> &entries, const std::optional<HostDiagnosticFile> &file) {
            if (file.has_value())
                entries.push_back(
                    {.sourcePath = file->sourcePath, .archivePath = file->archivePath, .optional = true, .redactSensitiveText = true});
        }
    }  // namespace

    /** @copydoc HostObservabilitySession::Start */
    std::unique_ptr<HostObservabilitySession> HostObservabilitySession::Start(HostObservabilityConfiguration configuration) {
        if (configuration.summaries.size() > 8 || std::ranges::any_of(configuration.summaries, [](const auto provider) {
            return provider == nullptr;
        }))
            return nullptr;
        if (bool expected = false; !HostSessionActive().compare_exchange_strong(expected, true))
            return nullptr;
        configuration.logging.logDirectory = ResolveLogDirectory(configuration.logging.logDirectory);
        if (configuration.logging.logDirectory.empty() || !configuration.logging.logDirectory.is_absolute()) {
            HostSessionActive().store(false);
            return nullptr;
        }
        if (!Log::Logger::Init(configuration.logging)) {
            HostSessionActive().store(false);
            return nullptr;
        }

        auto session = std::make_unique<HostObservabilitySession>(ConstructionToken{}, std::move(configuration));
        const HostObservabilityIdentity &identity = session->configuration_.identity;
        const std::array fields{
            Telemetry::Field{.key = "process.role", .value = identity.processRole},
            Telemetry::Field{.key = "engine.version", .value = identity.engineVersion},
            Telemetry::Field{.key = "build.configuration", .value = identity.buildConfiguration},
            Telemetry::Field{.key = "source.revision", .value = identity.sourceRevision},
            Telemetry::Field{.key = "os.name", .value = OperatingSystemName()},
            Telemetry::Field{.key = "cpu.architecture", .value = ArchitectureName()},
            Telemetry::Field{.key = "renderer.backend", .value = identity.rendererBackend},
            Telemetry::Field{.key = "device.name", .value = identity.deviceName},
            Telemetry::Field{.key = "project.id", .value = identity.projectId},
            Telemetry::Field{.key = "project.version", .value = identity.projectVersion},
        };
        const std::uint64_t acceptedBefore = Log::Logger::Statistics().acceptedRecords;
        for (std::size_t attempt = 0; attempt < 64 && Log::Logger::Statistics().acceptedRecords == acceptedBefore; ++attempt) {
            Log::Logger::Write("observability.startup", Log::Level::Info, "Host identity snapshot", fields);
            std::this_thread::yield();
        }
        return session;
    }

    /** @copydoc HostObservabilitySession::~HostObservabilitySession */
    HostObservabilitySession::~HostObservabilitySession() {
        Shutdown();
    }

    /** @copydoc HostObservabilitySession::GenerateDiagnosticBundle */
    Result<Diagnostics::DiagnosticBundleSummary> HostObservabilitySession::GenerateDiagnosticBundle(
        const HostDiagnosticBundleRequest &request) const {
        static_cast<void>(Log::Logger::Flush());

        Diagnostics::DiagnosticBundleRequest bundle;
        bundle.outputPath = request.outputPath;
        bundle.maxInputBytes = request.maxInputBytes;
        bundle.metadata = request.metadata;
        AppendHostIdentity(bundle.metadata, configuration_);

        const std::filesystem::path &directory = configuration_.logging.logDirectory;
        const std::string &baseName = configuration_.logging.baseName;
        bundle.entries.push_back({.sourcePath = directory / std::format("{}.jsonl", baseName),
                                  .archivePath = std::format("logs/{}.jsonl", baseName),
                                  .optional = true,
                                  .redactSensitiveText = true});
        bundle.entries.push_back({.sourcePath = directory / std::format("{}.operations.jsonl", baseName),
                                  .archivePath = std::format("history/{}.operations.jsonl", baseName),
                                  .optional = true,
                                  .redactSensitiveText = true});
        for (std::size_t index = 1; index <= configuration_.logging.maxRolledFiles; ++index) {
            const std::string suffix = std::format(".{}", index);
            bundle.entries.push_back({.sourcePath = directory / std::format("{}{}.jsonl", baseName, suffix),
                                      .archivePath = std::format("logs/{}{}.jsonl", baseName, suffix),
                                      .optional = true,
                                      .redactSensitiveText = true});
            bundle.entries.push_back({.sourcePath = directory / std::format("{}{}.operations.jsonl", baseName, suffix),
                                      .archivePath = std::format("history/{}{}.operations.jsonl", baseName, suffix),
                                      .optional = true,
                                      .redactSensitiveText = true});
        }
        bundle.entries.push_back({.sourcePath = directory / std::format("{}.session.json", baseName),
                                  .archivePath = "metadata/session.json",
                                  .optional = true,
                                  .redactSensitiveText = true});
        bundle.entries.push_back({.sourcePath = directory / std::format("{}.shutdown.json", baseName),
                                  .archivePath = "metadata/shutdown.json",
                                  .optional = true,
                                  .redactSensitiveText = true});
        if (const auto summaries = AppendConfiguredSummaries(bundle, configuration_.summaries); summaries.HasError())
            return Result<Diagnostics::DiagnosticBundleSummary>::Failure(summaries.ErrorValue());
        AppendOptionalEntry(bundle.entries, request.crashMetadata);
        AppendOptionalEntry(bundle.entries, request.configuration);
        AppendOptionalEntry(bundle.entries, request.packageSummary);
        return Diagnostics::GenerateDiagnosticBundle(bundle);
    }

    /** @copydoc HostObservabilitySession::Shutdown */
    void HostObservabilitySession::Shutdown() noexcept {
        if (!active_)
            return;
        active_ = false;
        Log::Logger::Shutdown();
        HostSessionActive().store(false);
    }

    /** @copydoc HostObservabilitySession::LogDirectory */
    const std::filesystem::path &HostObservabilitySession::LogDirectory() const noexcept {
        return configuration_.logging.logDirectory;
    }

    HostObservabilitySession::HostObservabilitySession(ConstructionToken, HostObservabilityConfiguration configuration)
        : configuration_(std::move(configuration)) {}

}  // namespace Horo::Application
