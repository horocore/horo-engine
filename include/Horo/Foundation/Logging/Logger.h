#pragma once

#include "Horo/Foundation/Logging/LogContext.h"
#include "Horo/Foundation/Logging/LogLevel.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Log {
    class StructuredLogStore;

    /** @brief Process-owned asynchronous logger configuration. */
    struct LoggerConfiguration {
        std::filesystem::path logDirectory;              /**< Absolute process-owned persistence directory. */
        std::string baseName{"horo"};                    /**< Stable artifact base name without an extension. */
        std::string hostName;                            /**< Product/process role; defaults to @ref baseName. */
        std::string hostVersion;                         /**< Optional application or engine version. */
        std::size_t queueCapacity{4096};                 /**< Fixed common-dispatcher record capacity. */
        std::uintmax_t maxFileBytes{8U * 1024U * 1024U}; /**< Maximum bytes per active JSONL segment. */
        std::size_t maxRolledFiles{5};                   /**< Maximum retained inactive JSONL segments. */
        Telemetry::OverflowPolicy overflowPolicy{Telemetry::OverflowPolicy::DropNewest}; /**< Saturation policy. */
        std::chrono::milliseconds shutdownTimeout{std::chrono::seconds{5}};              /**< Bounded drain deadline. */
        std::chrono::milliseconds sinkFlushInterval{std::chrono::seconds{1}};            /**< Periodic sink flush cadence. */
        Level minimumEventSeverity{Level::Trace};                                        /**< Common log/event severity gate. */
        std::vector<std::string> subsystemPrefixes;                                      /**< Optional hierarchical subsystem allowlist. */
        Telemetry::MetricCollectionLevel metricCollectionLevel{Telemetry::MetricCollectionLevel::Core}; /**< Host metric policy. */
        std::vector<std::shared_ptr<Telemetry::ISink>> additionalSinks; /**< Host-composed local or external consumers. */
        bool writeSessionMarkers{true};                                 /**< Whether session and clean-shutdown markers are persisted. */
        bool echoToStderr{
#ifndef NDEBUG
            true
#else
            false
#endif
        }; /**< Whether persisted log records are mirrored to standard error. */
    };

    /** @brief Monotonic counters describing asynchronous logger health. */
    struct LoggerStatistics {
        std::uint64_t acceptedRecords{};
        std::uint64_t writtenRecords{};
        std::uint64_t droppedRecords{};
        std::uint64_t emergencyRecords{};
    };

    /**
     * @brief Minimal structured logger with JSONL file output.
     *
     * All editor, CLI, and game hosts share this logger. It supports
     * runtime severity filtering and writes one JSON
     * object per line to a rotating file in the platform log directory.
     *
     * Usage:
     *   Logger::Init("~/.horo/logs", "horo-editor");
     *   HORO_LOG_INFO("editor.startup", "Editor initialised");
     *   HORO_LOG_DEBUG("editor.modal.settings", "Theme changed to %s", name.c_str());
     *
     * Disabled levels do not evaluate format arguments.
     */
    class Logger {
    public:
        Logger(const Logger &) = delete;
        Logger &operator=(const Logger &) = delete;
        Logger(Logger &&) = delete;
        Logger &operator=(Logger &&) = delete;

        /**
         * @brief Initialises the global logger with default queue and rolling limits.
         * @param logDir Absolute log directory; a leading tilde is expanded for legacy callers.
         * @param baseName Stable file base name without an extension.
         */
        static void Init(std::string_view logDir, std::string_view baseName);

        /**
         * @brief Initialises the process logger with explicit queue and rolling limits.
         * @param configuration Validated process-owned logger configuration.
         * @return True when the writer was started; false when configuration or file creation failed.
         */
        [[nodiscard]] static bool Init(const LoggerConfiguration &configuration);

        /** @brief Shuts down the logger, flushes and closes the file. */
        static void Shutdown();

        /**
         * @brief Waits until records accepted before this call are persisted.
         * @param timeout Maximum time to wait for the writer.
         * @return True when the flush watermark was persisted before the timeout.
         */
        [[nodiscard]] static bool Flush(std::chrono::milliseconds timeout = std::chrono::seconds{5});

        /**
         * @brief Returns the active global minimum level.
         * @return Current process severity threshold.
         */
        [[nodiscard]] static Level GetLevel() noexcept;

        /**
         * @brief Sets the active global minimum level.
         * @param level New process severity threshold.
         */
        static void SetLevel(Level level) noexcept;

        /**
         * @brief Returns whether a severity is currently enabled without allocating.
         * @param level Candidate record severity.
         * @return True when the record passes the process threshold.
         */
        [[nodiscard]] static bool IsEnabled(Level level) noexcept;

        /**
         * @brief Returns a snapshot of asynchronous ingestion and writer counters.
         * @return Monotonic process-lifetime logger health counters.
         */
        [[nodiscard]] static LoggerStatistics Statistics() noexcept;

        /**
         * @brief Installs the optional process-owned bounded in-memory sink.
         * @param store Shared store retained until replacement or logger shutdown.
         */
        static void SetStructuredLogStore(std::shared_ptr<StructuredLogStore> store);

        /**
         * @brief Logs a structured record if `level` passes the current filter.
         *
         * @param category  Stable dotted category (e.g. "editor.startup").
         * @param level     Severity of this record.
         * @param message   Pre-formatted message. Format arguments are evaluated
         *                  only when the record passes the level gate.
         */
        static void Write(std::string_view category, Level level, std::string_view message);

        /**
         * @brief Logs a record with separately queryable typed fields.
         * @param category Stable dotted category and owning subsystem tag.
         * @param level Severity of this record.
         * @param message Human-readable pre-formatted message.
         * @param fields Bounded record-local fields copied before asynchronous delivery.
         */
        static void Write(std::string_view category, Level level, std::string_view message, std::span<const Telemetry::Field> fields);

        /**
         * @brief Writes an emergency record directly to standard error, bypassing filters and queued sinks.
         * @param category Stable hierarchical record category.
         * @param level Record severity, normally `Critical` for a fail-fast condition.
         * @param message Human-readable context, truncated to the emergency output bound.
         * @note This is a reporting path only and never returns an operation result.
         */
        static void WriteEmergency(std::string_view category, Level level, std::string_view message) noexcept;

        /** @brief Emits the startup system-information snapshot. */
        static void DumpStartupInfo();

    private:
        Logger() = delete;
    };

}  // namespace Horo::Log

// ── Convenience macros ──────────────────────────────────────────────────
// Disabled levels compile to nothing (no format argument evaluation).

#define HORO_LOG_TRACE(cat, ...)                                                                                                           \
    do {                                                                                                                                   \
        if (::Horo::Log::Logger::IsEnabled(::Horo::Log::Level::Trace))                                                                     \
            ::Horo::Log::Logger::Write((cat), ::Horo::Log::Level::Trace, ::Horo::Log::FormatArgs(__VA_ARGS__));                            \
    } while (false)

#define HORO_LOG_DEBUG(cat, ...)                                                                                                           \
    do {                                                                                                                                   \
        if (::Horo::Log::Logger::IsEnabled(::Horo::Log::Level::Debug))                                                                     \
            ::Horo::Log::Logger::Write((cat), ::Horo::Log::Level::Debug, ::Horo::Log::FormatArgs(__VA_ARGS__));                            \
    } while (false)

#define HORO_LOG_INFO(cat, ...)                                                                                                            \
    do {                                                                                                                                   \
        if (::Horo::Log::Logger::IsEnabled(::Horo::Log::Level::Info))                                                                      \
            ::Horo::Log::Logger::Write((cat), ::Horo::Log::Level::Info, ::Horo::Log::FormatArgs(__VA_ARGS__));                             \
    } while (false)

#define HORO_LOG_WARN(cat, ...)                                                                                                            \
    do {                                                                                                                                   \
        if (::Horo::Log::Logger::IsEnabled(::Horo::Log::Level::Warn))                                                                      \
            ::Horo::Log::Logger::Write((cat), ::Horo::Log::Level::Warn, ::Horo::Log::FormatArgs(__VA_ARGS__));                             \
    } while (false)

#define HORO_LOG_ERROR(cat, ...)                                                                                                           \
    do {                                                                                                                                   \
        if (::Horo::Log::Logger::IsEnabled(::Horo::Log::Level::Error))                                                                     \
            ::Horo::Log::Logger::Write((cat), ::Horo::Log::Level::Error, ::Horo::Log::FormatArgs(__VA_ARGS__));                            \
    } while (false)

#define HORO_LOG_CRITICAL(cat, ...)                                                                                                        \
    do {                                                                                                                                   \
        if (::Horo::Log::Logger::IsEnabled(::Horo::Log::Level::Critical))                                                                  \
            ::Horo::Log::Logger::Write((cat), ::Horo::Log::Level::Critical, ::Horo::Log::FormatArgs(__VA_ARGS__));                         \
    } while (false)

namespace Horo::Log {

    /**
     * @brief Simple format helper — avoids non-POD variadic UB.
     *
     * The format must be a compile-time character array. Arguments accept fundamental types and `const char *` only.
     * For std::string, pass `.c_str()` explicitly.
     */
    template <std::size_t Size, typename... Args> [[nodiscard]] std::string FormatArgs(const char (&fmt)[Size], Args &&...args) {
        // The array-reference contract accepts compile-time format strings only.
        const int size = std::snprintf(nullptr, 0, fmt, std::forward<Args>(args)...);  // NOSONAR
        if (size <= 0)
            return {};
        std::string result(static_cast<std::size_t>(size) + 1, '\0');
        std::snprintf(result.data(), result.size(), fmt, std::forward<Args>(args)...);  // NOSONAR
        result.pop_back();
        return result;
    }

    template <std::size_t Size> [[nodiscard]] std::string FormatArgs(const char (&msg)[Size]) {
        return {msg};
    }

}  // namespace Horo::Log

// ── Short aliases — prefer these for new call sites ───────────────────────
// The HORO_LOG_* macros remain for backward compatibility.
// clang-format off
#define LOG_TRACE    HORO_LOG_TRACE
#define LOG_DEBUG    HORO_LOG_DEBUG
#define LOG_INFO     HORO_LOG_INFO
#define LOG_WARN     HORO_LOG_WARN
#define LOG_ERROR    HORO_LOG_ERROR
#define LOG_CRITICAL HORO_LOG_CRITICAL
// clang-format on
