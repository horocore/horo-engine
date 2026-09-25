/** @copydoc Logger.h */

#include "Horo/Foundation/Logging/Logger.h"

#include "Horo/Foundation/Diagnostics/OperationHistory.h"
#include "Horo/Foundation/Logging/LogContext.h"
#include "Horo/Foundation/Logging/StructuredLogStore.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Horo::Log {
    namespace {
        /** @brief Dedicated failure raised by the JSONL persistence boundary. */
        class LoggerPersistenceError final : public std::runtime_error {
        public:
            using std::runtime_error::runtime_error;
        };

        /** @brief Process logger state accessed through one function-local owner. */
        struct LoggerState {
            using AtomicLevel = std::atomic<Level>;

            [[nodiscard]] std::shared_ptr<StructuredLogStore> LoadStore() const noexcept {
                return std::atomic_load(&store);
            }

            void SetStore(std::shared_ptr<StructuredLogStore> replacement) noexcept {
                std::atomic_store(&store, std::move(replacement));
            }

            AtomicLevel level{Level::Info};
            std::atomic<std::uint64_t> accepted{};
            std::atomic<std::uint64_t> written{};
            std::atomic<std::uint64_t> dropped{};
            std::atomic<std::uint64_t> emergency{};
            std::atomic<bool> ownsRuntime{};
            std::mutex lifecycleMutex;
            std::shared_ptr<StructuredLogStore> store;
            std::filesystem::path shutdownMarkerPath;
            std::string sessionId;
            bool writeSessionMarkers{};
            std::atomic<std::uint64_t> sessionSequence{};
        };

        LoggerState &State() {
            static LoggerState state;
            return state;
        }

        std::uint64_t ProcessId() noexcept;

        [[nodiscard]] bool IsSafeBaseName(const std::string_view name) noexcept {
            return !name.empty() && name.size() <= 64 && std::ranges::none_of(name, [](const char character) {
                return character == '/' || character == '\\' || character == '.' || character == ':';
            });
        }

        [[nodiscard]] bool IsSafeResolvedPath(const std::filesystem::path &path) noexcept {
            return !path.empty() && path.is_absolute() && std::ranges::none_of(path, [](const std::filesystem::path &component) {
                return component == "..";
            });
        }

        /** @brief Resolves an absolute log directory and creates it when absent. */
        std::filesystem::path ResolveLogDirectory(const std::filesystem::path &input) {
            std::string value = input.string();
            if (!value.empty() && value.front() == '~') {
                const char *home = std::getenv("HOME");
                if (home == nullptr)
                    home = std::getenv("USERPROFILE");
                if (home == nullptr)
                    return {};
                value = std::string{home} + value.substr(1);
            }

            const std::filesystem::path path{value};
            if (!IsSafeResolvedPath(path))
                return {};
            std::error_code error;
            const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
            if (error || !IsSafeResolvedPath(canonical))
                return {};
            std::filesystem::create_directories(canonical, error);
            return error ? std::filesystem::path{} : canonical;
        }

        /** @brief Formats a UTC timestamp as ISO-8601 with milliseconds. */
        std::string FormatUtc(const std::chrono::system_clock::time_point timestamp) {
            const auto time = std::chrono::system_clock::to_time_t(timestamp);
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()) % 1000;
            std::tm parts{};
#if defined(_WIN32)
            gmtime_s(&parts, &time);
#else
            gmtime_r(&time, &parts);
#endif
            return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                               parts.tm_hour, parts.tm_min, parts.tm_sec, milliseconds.count());
        }

        /** @brief Appends a JSON-escaped string without creating an intermediate value. */
        void AppendJsonEscaped(std::string &output, const std::string_view value) {
            static constexpr std::array kHex{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
            for (const char rawCharacter : value) {
                const auto byteValue = static_cast<std::byte>(rawCharacter);
                const auto character = std::to_integer<std::uint8_t>(byteValue);
                switch (rawCharacter) {
                    case '"':
                        output += R"(\")";
                        break;
                    case '\\':
                        output += R"(\\)";
                        break;
                    case '\n':
                        output += "\\n";
                        break;
                    case '\r':
                        output += "\\r";
                        break;
                    case '\t':
                        output += "\\t";
                        break;
                    case '\b':
                        output += "\\b";
                        break;
                    case '\f':
                        output += "\\f";
                        break;
                    default:
                        if (character < 0x20U) {
                            output += "\\u00";
                            output += kHex[std::to_integer<std::size_t>((byteValue >> 4U) & std::byte{0x0FU})];
                            output += kHex[std::to_integer<std::size_t>(byteValue & std::byte{0x0FU})];
                        } else {
                            output += rawCharacter;
                        }
                }
            }
        }

        /** @brief Atomically replaces one small process-session metadata file. */
        bool WriteMarkerAtomically(const std::filesystem::path &path, const std::string_view content) {
            if (!IsSafeResolvedPath(path) || !IsSafeResolvedPath(path.parent_path()))
                return false;
            std::filesystem::path temporary = path;
            temporary += std::format(".tmp-{}", ProcessId());
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output)
                    return false;
                output.write(content.data(), static_cast<std::streamsize>(content.size()));
                output.flush();
                if (!output)
                    return false;
            }
            std::error_code error;
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
            if (!error)
                return true;
            std::filesystem::remove(temporary, error);
            return false;
        }

        /** @brief Reads a bounded marker and extracts its restricted session identifier. */
        std::optional<std::string> ReadMarkerSessionId(const std::filesystem::path &path) {
            std::error_code error;
            if (const std::uintmax_t size = std::filesystem::file_size(path, error); error || size == 0 || size > 16U * 1024U)
                return std::nullopt;
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return std::nullopt;
            const std::string content{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
            constexpr std::string_view prefix{R"("sessionId":")"};
            const std::size_t start = content.find(prefix);
            if (start == std::string::npos)
                return std::nullopt;
            const std::size_t valueStart = start + prefix.size();
            const std::size_t end = content.find('"', valueStart);
            if (end == std::string::npos || end == valueStart)
                return std::nullopt;
            const std::string value = content.substr(valueStart, end - valueStart);
            for (const unsigned char character : value) {
                if (character != '-' && (character < '0' || character > '9'))
                    return std::nullopt;
            }
            return value;
        }

        /** @brief Creates a collision-resistant process-local session identifier. */
        std::string CreateSessionId() {
            const auto timestamp =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            return std::format("{}-{}-{}", timestamp, ProcessId(), State().sessionSequence.fetch_add(1));
        }

        /** @brief Writes immutable current-session identity without private machine data. */
        bool WriteSessionMarker(const LoggerConfiguration &configuration, const std::string_view sessionId,
                                const bool previousCleanShutdown) {
            std::string content;
            content.reserve(configuration.hostName.size() + configuration.hostVersion.size() + sessionId.size() + 192U);
            content += R"({"schemaVersion":1,"sessionId":")";
            AppendJsonEscaped(content, sessionId);
            content += R"(","startedAt":")";
            AppendJsonEscaped(content, FormatUtc(std::chrono::system_clock::now()));
            content += R"(","host":")";
            AppendJsonEscaped(content, configuration.hostName.empty() ? configuration.baseName : configuration.hostName);
            content += R"(","version":")";
            AppendJsonEscaped(content, configuration.hostVersion);
            content += std::format(R"(","processId":{})", ProcessId());
            content += R"(,"previousCleanShutdown":)";
            content += previousCleanShutdown ? "true" : "false";
            content += "}\n";
            return WriteMarkerAtomically(configuration.logDirectory / (configuration.baseName + ".session.json"), content);
        }

        /** @brief Writes the marker proving the common dispatcher drained successfully. */
        bool WriteShutdownMarker(const std::filesystem::path &path, const std::string_view sessionId) {
            std::string content;
            content.reserve(sessionId.size() + 96U);
            content += R"({"schemaVersion":1,"sessionId":")";
            AppendJsonEscaped(content, sessionId);
            content += R"(","shutdownAt":")";
            AppendJsonEscaped(content, FormatUtc(std::chrono::system_clock::now()));
            content += R"(","clean":true})";
            content += '\n';
            return WriteMarkerAtomically(path, content);
        }

        /** @brief Appends one locale-independent JSON scalar. */
        void AppendFieldValue(std::string &output, const Telemetry::FieldValue &value) {
            std::visit([&output]<typename Value>(const Value &typed) {
                if constexpr (std::is_same_v<Value, bool>) {
                    output += typed ? "true" : "false";
                } else if constexpr (std::is_same_v<Value, std::string>) {
                    output += '"';
                    AppendJsonEscaped(output, typed);
                    output += '"';
                } else {
                    std::array<char, 64> buffer{};
                    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), typed);
                    if (error != std::errc{})
                        throw LoggerPersistenceError{"failed to format structured log field"};
                    output.append(buffer.data(), end);
                }
            }, value);
        }

        /** @brief Builds the legacy compact context presentation string. */
        std::string FormatPresentationContext(const LogContextSnapshot &context) {
            const auto fields = context.Fields();
            if (fields.empty())
                return {};
            std::string output{" ["};
            for (std::size_t index = 0; index < fields.size(); ++index) {
                if (index != 0)
                    output += ", ";
                output += fields[index].first;
                output += '=';
                output += fields[index].second;
            }
            output += ']';
            return output;
        }

        /** @brief Returns the current process identifier without exposing native types. */
        std::uint64_t ProcessId() noexcept {
#if defined(_WIN32)
            return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
            return static_cast<std::uint64_t>(getpid());
#endif
        }

        /** @brief Removes an incomplete trailing JSONL record left by an interrupted write. */
        bool RepairPartialTrailingRecord(const std::filesystem::path &path) {
            if (!IsSafeResolvedPath(path))
                return false;
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error || size == 0)
                return !error || !std::filesystem::exists(path);

            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            input.seekg(static_cast<std::streamoff>(size - 1U));
            char character{};
            input.get(character);
            if (character == '\n')
                return true;

            std::uintmax_t offset = size - 1U;
            while (offset > 0) {
                --offset;
                input.seekg(static_cast<std::streamoff>(offset));
                input.get(character);
                if (character == '\n') {
                    std::filesystem::resize_file(path, offset + 1U, error);  // NOSONAR
                    return !error;
                }
            }
            std::filesystem::resize_file(path, 0, error);  // NOSONAR
            return !error;
        }

        /** @brief Consumer-thread JSONL persistence sink with deterministic rolling. */
        class RotatingJsonlSink final : public Telemetry::ISink {
        public:
            explicit RotatingJsonlSink(LoggerConfiguration configuration)
                : configuration_(std::move(configuration)), path_(configuration_.logDirectory / (configuration_.baseName + ".jsonl")) {
                if (!RepairPartialTrailingRecord(path_))
                    throw LoggerPersistenceError{"failed to recover partial JSONL record"};
                file_ = std::fopen(path_.string().c_str(), "ab");  // NOSONAR
                if (file_ == nullptr)
                    throw LoggerPersistenceError{"failed to open JSONL log"};
                std::error_code error;
                currentBytes_ = std::filesystem::file_size(path_, error);
                if (error)
                    currentBytes_ = 0;
            }

            ~RotatingJsonlSink() override {
                if (file_ != nullptr)
                    std::fclose(file_);
            }

            RotatingJsonlSink(const RotatingJsonlSink &) = delete;
            RotatingJsonlSink &operator=(const RotatingJsonlSink &) = delete;
            RotatingJsonlSink(RotatingJsonlSink &&) = delete;
            RotatingJsonlSink &operator=(RotatingJsonlSink &&) = delete;

            void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *) override {
                const auto *log = std::get_if<Telemetry::LogRecord>(&record.payload);
                if (log == nullptr)
                    return;
                const std::string line = FormatRecord(record, *log);
                if (line.size() > configuration_.maxFileBytes)
                    throw LoggerPersistenceError{"structured log record exceeds configured file limit"};
                if (currentBytes_ != 0 && currentBytes_ + line.size() > configuration_.maxFileBytes)
                    RollFiles();
                if (file_ == nullptr)
                    throw LoggerPersistenceError{"JSONL log is unavailable"};
                const std::size_t written = std::fwrite(line.data(), 1, line.size(), file_);
                if (written != line.size() || std::fflush(file_) != 0)
                    throw LoggerPersistenceError{"failed to persist JSONL log record"};
                currentBytes_ += written;
                State().written.fetch_add(1);
                if (configuration_.echoToStderr)
                    std::fprintf(stderr, "[%s] %s: %s\n", ToString(log->severity), log->category.c_str(), log->message.c_str());
            }

            void Flush() override {
                if (file_ != nullptr && std::fflush(file_) != 0)
                    throw LoggerPersistenceError{"failed to flush JSONL log"};
            }

        private:
            [[nodiscard]] std::string FormatRecord(const Telemetry::Record &record, const Telemetry::LogRecord &log) const {
                std::string output;
                output.reserve(log.category.size() + log.message.size() + log.fields.size() * 32U + record.context.Fields().size() * 32U +
                               256U);
                output += R"({"schemaVersion":1,"timestamp":")";
                output += FormatUtc(record.timestampUtc);
                output += R"(","elapsedMs":)";
                const double elapsed = std::chrono::duration<double, std::milli>(record.monotonicTime - startedAt_).count();
                AppendFieldValue(output, elapsed);
                output += R"(,"sequence":)";
                AppendFieldValue(output, record.sequence);
                output += R"(,"level":")";
                output += ToString(log.severity);
                output += R"(","subsystem":")";
                AppendJsonEscaped(output, record.subsystem);
                output += R"(","category":")";
                AppendJsonEscaped(output, log.category);
                output += R"(","message":")";
                AppendJsonEscaped(output, log.message);
                output += R"(","process":{"pid":)";
                AppendFieldValue(output, ProcessId());
                output += R"(},"thread":{"id":)";
                AppendFieldValue(output, record.threadId);
                output += '}';
                if (!log.fields.empty()) {
                    output += R"(,"fields":{)";
                    for (std::size_t index = 0; index < log.fields.size(); ++index) {
                        if (index != 0)
                            output += ',';
                        output += '"';
                        AppendJsonEscaped(output, log.fields[index].key);
                        output += R"(":)";
                        AppendFieldValue(output, log.fields[index].value);
                    }
                    output += '}';
                }
                if (const auto context = record.context.Fields(); !context.empty()) {
                    output += R"(,"context":{)";
                    for (std::size_t index = 0; index < context.size(); ++index) {
                        if (index != 0)
                            output += ',';
                        output += '"';
                        AppendJsonEscaped(output, context[index].first);
                        output += R"(":")";
                        AppendJsonEscaped(output, context[index].second);
                        output += '"';
                    }
                    output += '}';
                }
                output += "}\n";
                return output;
            }

            [[nodiscard]] std::filesystem::path RolledPath(const std::size_t index) const {
                return std::filesystem::path{std::format("{}.{}", path_.string(), index)};
            }

            void ReopenCurrentForAppend() noexcept {
                file_ = std::fopen(path_.string().c_str(), "ab");
                std::error_code error;
                currentBytes_ = std::filesystem::file_size(path_, error);
                if (error)
                    currentBytes_ = 0;
            }

            void RollFiles() {
                if (file_ != nullptr) {
                    std::fflush(file_);
                    std::fclose(file_);
                    file_ = nullptr;
                }

                std::error_code error;
                if (configuration_.maxRolledFiles == 0) {
                    std::filesystem::remove(path_, error);
                    if (error) {
                        ReopenCurrentForAppend();
                        throw LoggerPersistenceError{"failed to replace current JSONL log"};
                    }
                } else {
                    std::filesystem::remove(RolledPath(configuration_.maxRolledFiles), error);
                    if (error) {
                        ReopenCurrentForAppend();
                        throw LoggerPersistenceError{"failed to remove expired JSONL segment"};
                    }
                    for (std::size_t index = configuration_.maxRolledFiles; index > 1; --index) {
                        const auto source = RolledPath(index - 1U);
                        if (!std::filesystem::exists(source, error)) {
                            error.clear();
                            continue;
                        }
                        std::filesystem::rename(source, RolledPath(index), error);
                        if (error) {
                            ReopenCurrentForAppend();
                            throw LoggerPersistenceError{"failed to roll JSONL segment"};
                        }
                    }
                    if (std::filesystem::exists(path_, error)) {
                        error.clear();
                        std::filesystem::rename(path_, RolledPath(1), error);
                    }
                    if (error) {
                        ReopenCurrentForAppend();
                        throw LoggerPersistenceError{"failed to archive current JSONL log"};
                    }
                }

                file_ = std::fopen(path_.string().c_str(), "wb");
                if (file_ == nullptr)
                    throw LoggerPersistenceError{"failed to create replacement JSONL log"};
                currentBytes_ = 0;
            }

            LoggerConfiguration configuration_;
            std::filesystem::path path_;
            std::FILE *file_{};
            std::uintmax_t currentBytes_{};
            std::chrono::steady_clock::time_point startedAt_{std::chrono::steady_clock::now()};
        };

        /** @brief Consumer-thread adapter for the optional bounded editor log store. */
        class StructuredLogStoreSink final : public Telemetry::ISink {
        public:
            void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *) override {
                const auto *log = std::get_if<Telemetry::LogRecord>(&record.payload);
                if (log == nullptr)
                    return;
                const auto store = State().LoadStore();
                if (store == nullptr)
                    return;
                store->Append(StructuredLogRecord{.sequence = record.sequence,
                                                  .timestampUtc = record.timestampUtc,
                                                  .level = log->severity,
                                                  .category = log->category,
                                                  .message = log->message,
                                                  .context = FormatPresentationContext(record.context)});
            }

            void Flush() override {
                // The bounded in-memory store commits synchronously in Append.
            }
        };

        void EmergencyWrite(const Level level, const std::string_view category, const std::string_view message) {
            constexpr std::size_t kMaximumCategoryBytes = 128;
            constexpr std::size_t kMaximumMessageBytes = 4096;
            const std::string_view boundedCategory = category.substr(0, kMaximumCategoryBytes);
            const std::string_view boundedMessage = message.substr(0, kMaximumMessageBytes);
            State().emergency.fetch_add(1);
            std::fprintf(stderr, "[logger-emergency][%s] %.*s: %.*s\n", ToString(level), static_cast<int>(boundedCategory.size()),
                         boundedCategory.empty() ? "" : boundedCategory.data(), static_cast<int>(boundedMessage.size()),
                         boundedMessage.empty() ? "" : boundedMessage.data());
            static_cast<void>(std::fflush(stderr));
        }

        Level ParseEnvironmentLevel(const Level fallback) {
            using enum Level;
            const char *environment = std::getenv("HORO_LOG_LEVEL");
            if (environment == nullptr)
                return fallback;
            const std::string_view value{environment};
            if (value == "trace")
                return Trace;
            if (value == "debug")
                return Debug;
            if (value == "info")
                return Info;
            if (value == "warn")
                return Warn;
            if (value == "error")
                return Error;
            if (value == "critical")
                return Critical;
            if (value == "off")
                return Off;
            return fallback;
        }

        bool EmitLog(const std::string_view category, const Level level, const std::string_view message,
                     const std::span<const Telemetry::Field> fields, LogContextSnapshot context) {
            if (Telemetry::LogRecord payload{.severity = level,
                                             .category = std::string{category},
                                             .message = std::string{message},
                                             .fields = {fields.begin(), fields.end()}};
                Telemetry::Runtime::EmitRecord(
                    Telemetry::Record{.subsystem = std::string{category}, .context = std::move(context), .payload = std::move(payload)})) {
                State().accepted.fetch_add(1);
                return true;
            }
            return false;
        }
    }  // namespace

    /** @copydoc Logger::Init(std::string_view, std::string_view) */
    void Logger::Init(const std::string_view logDir, const std::string_view baseName) {
        LoggerConfiguration configuration{.logDirectory = std::filesystem::path{logDir}, .baseName = std::string{baseName}};
        static_cast<void>(Init(configuration));
    }

    /** @copydoc Logger::Init(const LoggerConfiguration&) */
    bool Logger::Init(const LoggerConfiguration &configuration) {
        if (!IsSafeBaseName(configuration.baseName))
            return false;
        LoggerConfiguration resolved = configuration;
        resolved.logDirectory = ResolveLogDirectory(configuration.logDirectory);
        if (resolved.logDirectory.empty() || resolved.baseName.empty() || !IsSafeBaseName(resolved.baseName) ||
            resolved.queueCapacity == 0 || resolved.maxFileBytes == 0 || resolved.shutdownTimeout <= std::chrono::milliseconds::zero() ||
            resolved.sinkFlushInterval <= std::chrono::milliseconds::zero())
            return false;

        LoggerState &state = State();
        std::lock_guard lock(state.lifecycleMutex);
        if (state.ownsRuntime.load())
            return true;

        std::shared_ptr<Telemetry::ISink> fileSink;
        try {
            fileSink = std::make_shared<RotatingJsonlSink>(resolved);
        } catch (const LoggerPersistenceError &error) {
            std::fprintf(stderr, "[Logger] failed to open log output (errno=%d): %s (%s)\n", errno, std::strerror(errno), error.what());
            return false;
        } catch (const std::bad_alloc &error) {
            std::fprintf(stderr, "[Logger] failed to open log output (errno=%d): %s (%s)\n", errno, std::strerror(errno), error.what());
            return false;
        }

        std::vector<std::shared_ptr<Telemetry::ISink>> sinks;
        sinks.reserve(3U + resolved.additionalSinks.size());
        sinks.push_back(std::move(fileSink));
        sinks.push_back(std::make_shared<StructuredLogStoreSink>());
        if (const auto historySink = Diagnostics::OperationHistorySink::Create({.directory = resolved.logDirectory,
                                                                                .baseName = std::format("{}.operations", resolved.baseName),
                                                                                .maxFileBytes = resolved.maxFileBytes,
                                                                                .maxRolledFiles = resolved.maxRolledFiles,
                                                                                .maxRecoveredRecords = 1024});
            historySink != nullptr)
            sinks.push_back(historySink);
        else
            std::fprintf(stderr, "[Logger] operation history sink is unavailable; continuing without persistent history\n");
        sinks.insert(sinks.end(), resolved.additionalSinks.begin(), resolved.additionalSinks.end());
        if (!Telemetry::Runtime::Initialize({.queueCapacity = resolved.queueCapacity,
                                             .overflowPolicy = resolved.overflowPolicy,
                                             .shutdownTimeout = resolved.shutdownTimeout,
                                             .sinkFlushInterval = resolved.sinkFlushInterval,
                                             .minimumEventSeverity = resolved.minimumEventSeverity,
                                             .subsystemPrefixes = std::move(resolved.subsystemPrefixes),
                                             .metricCollectionLevel = resolved.metricCollectionLevel,
                                             .enabled = true},
                                            std::move(sinks)))
            return false;

        const std::filesystem::path sessionMarkerPath = resolved.logDirectory / (resolved.baseName + ".session.json");
        const std::filesystem::path shutdownMarkerPath = resolved.logDirectory / (resolved.baseName + ".shutdown.json");
        const std::optional<std::string> previousSessionId = ReadMarkerSessionId(sessionMarkerPath);
        const std::optional<std::string> cleanSessionId = ReadMarkerSessionId(shutdownMarkerPath);
        const bool previousCleanShutdown = previousSessionId.has_value() && cleanSessionId == previousSessionId;
        const std::string sessionId = CreateSessionId();
        if (resolved.writeSessionMarkers && !WriteSessionMarker(resolved, sessionId, previousCleanShutdown)) {
            static_cast<void>(Telemetry::Runtime::Shutdown());
            std::fprintf(stderr, "[Logger] failed to persist session identity\n");
            return false;
        }

        state.level.store(ParseEnvironmentLevel(state.level.load()));
        state.shutdownMarkerPath = shutdownMarkerPath;
        state.sessionId = sessionId;
        state.writeSessionMarkers = resolved.writeSessionMarkers;
        state.ownsRuntime.store(true);
        const std::array startupFields{
            Telemetry::Field{.key = "session.id", .value = sessionId},
            Telemetry::Field{.key = "host.name", .value = resolved.hostName.empty() ? resolved.baseName : resolved.hostName},
            Telemetry::Field{.key = "host.version", .value = resolved.hostVersion},
            Telemetry::Field{.key = "previous.clean_shutdown", .value = previousCleanShutdown},
            Telemetry::Field{.key = "process.id", .value = ProcessId()},
        };
        static_cast<void>(EmitLog("observability.startup", Level::Info, "Observability runtime initialised", startupFields, {}));
        return true;
    }

    /** @copydoc Logger::Shutdown */
    void Logger::Shutdown() {
        LoggerState &state = State();
        std::lock_guard lock(state.lifecycleMutex);
        if (state.ownsRuntime.exchange(false)) {
            static_cast<void>(EmitLog("observability.shutdown", Level::Info, "Observability runtime stopping", {}, {}));
            const bool clean = Telemetry::Runtime::Shutdown();
            if (clean && state.writeSessionMarkers && !WriteShutdownMarker(state.shutdownMarkerPath, state.sessionId))
                std::fprintf(stderr, "[Logger] failed to persist clean-shutdown marker\n");
        }
        state.shutdownMarkerPath.clear();
        state.sessionId.clear();
        state.writeSessionMarkers = false;
        state.SetStore({});
    }

    /** @copydoc Logger::Flush */
    bool Logger::Flush(const std::chrono::milliseconds timeout) {
        return Telemetry::Runtime::Flush(timeout);
    }

    /** @copydoc Logger::GetLevel */
    Level Logger::GetLevel() noexcept {
        return State().level.load();
    }

    /** @copydoc Logger::SetLevel */
    void Logger::SetLevel(const Level level) noexcept {
        State().level.store(level);
    }

    /** @copydoc Logger::IsEnabled */
    bool Logger::IsEnabled(const Level level) noexcept {
        const Level minimum = State().level.load();
        return minimum != Level::Off && level >= minimum;
    }

    /** @copydoc Logger::Statistics */
    LoggerStatistics Logger::Statistics() noexcept {
        const LoggerState &state = State();
        return {.acceptedRecords = state.accepted.load(),
                .writtenRecords = state.written.load(),
                .droppedRecords = state.dropped.load(),
                .emergencyRecords = state.emergency.load()};
    }

    /** @copydoc Logger::SetStructuredLogStore */
    void Logger::SetStructuredLogStore(std::shared_ptr<StructuredLogStore> store) {
        State().SetStore(std::move(store));
    }

    /** @copydoc Logger::Write */
    void Logger::Write(const std::string_view category, const Level level, const std::string_view message) {
        Write(category, level, message, {});
    }

    /** @copydoc Logger::Write */
    void Logger::Write(const std::string_view category, const Level level, const std::string_view message,
                       const std::span<const Telemetry::Field> fields) {
        if (!IsEnabled(level))
            return;

        LogContextSnapshot context = CaptureLogContext();
        if (Telemetry::Runtime::IsEnabled()) {
            if (EmitLog(category, level, message, fields, std::move(context)))
                return;
            State().dropped.fetch_add(1);
            if (level >= Level::Warn)
                EmergencyWrite(level, category, message);
            return;
        }

        const auto store = State().LoadStore();
        if (store != nullptr) {
            const std::uint64_t sequence = State().accepted.fetch_add(1) + 1U;
            store->Append(StructuredLogRecord{.sequence = sequence,
                                              .timestampUtc = std::chrono::system_clock::now(),
                                              .level = level,
                                              .category = std::string{category},
                                              .message = std::string{message},
                                              .context = FormatPresentationContext(context)});
            State().written.fetch_add(1);
        }
    }

    /** @copydoc Logger::WriteEmergency */
    void Logger::WriteEmergency(const std::string_view category, const Level level, const std::string_view message) noexcept {
        EmergencyWrite(level, category, message);
    }

    /** @copydoc Logger::DumpStartupInfo */
    void Logger::DumpStartupInfo() {
        LOG_INFO("observability.startup", "Process started");
#if defined(__APPLE__)
        LOG_INFO("observability.startup", "Platform: macOS");
#elif defined(__linux__)
        LOG_INFO("observability.startup", "Platform: Linux");
#elif defined(_WIN32)
        LOG_INFO("observability.startup", "Platform: Windows");
#endif
        LOG_INFO("observability.startup", "Logical cores: %u", std::thread::hardware_concurrency());
#ifdef NDEBUG
        LOG_INFO("observability.startup", "Build: Release");
#else
        LOG_INFO("observability.startup", "Build: Debug");
#endif
        LOG_INFO("observability.startup", "Log level: %s", ToString(GetLevel()));
    }
}  // namespace Horo::Log
