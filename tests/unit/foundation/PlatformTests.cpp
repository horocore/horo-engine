#include "Horo/Foundation/Platform.h"
#include "Horo/Platform/ConfigurationFileStore.h"
#include "Horo/Platform/ExternalProcess.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
    const Horo::ErrorCodeDescriptor kInjectedStoreFailure{
        .domain = Horo::ErrorDomainId{"test.configuration-store"},
        .code = Horo::ErrorCode{"injected_failure"},
        .defaultSeverity = Horo::ErrorSeverity::Error,
        .summary = "Injected configuration-store failure.",
    };

    class RecordingFileSystem final : public Horo::FileSystem {
    public:
        [[nodiscard]] bool Exists(const std::filesystem::path &path) const override {
            lastPath = path;
            return exists;
        }

        bool exists{true};
        mutable std::filesystem::path lastPath;
    };

    class FixedProcessService final : public Horo::ProcessService {
    public:
        [[nodiscard]] Horo::ProcessMetadata CurrentProcess() const override {
            return Horo::ProcessMetadata{.id = 42, .executableName = "test-host"};
        }

        [[nodiscard]] std::optional<std::string> EnvironmentValue(const std::string_view name) const override {
            if (name == "HORO_TEST")
                return std::string{"enabled"};
            return std::nullopt;
        }
    };

    [[nodiscard]] Horo::ConfigurationSnapshot BuildConfigurationSnapshot() {
        Horo::ConfigurationSchema schema;
        REQUIRE(schema
                    .Register({.key = Horo::SettingKey{"runtime.worker_count"},
                               .type = Horo::SettingValueType::Integer,
                               .defaultValue = std::int64_t{4},
                               .scope = Horo::SettingScope::Engine,
                               .reloadPolicy = Horo::ReloadPolicy::ProcessRestart,
                               .sensitivity = Horo::SettingSensitivity::Public})
                    .HasValue());
        REQUIRE(schema.Seal().HasValue());
        Horo::ConfigurationService service{std::move(schema)};
        return service.Snapshot();
    }

    class FaultingDurableFileSystem final : public Horo::DurableFileSystem {
    public:
        [[nodiscard]] Horo::Result<Horo::ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path,
                                                                                const std::string_view ownerMetadata) override {
            operations.push_back("lock:" + path.filename().string() + ":" + std::string{ownerMetadata});
            if (failLock)
                return Horo::Result<Horo::ExclusiveFileLock>::Failure(Horo::MakeError(kInjectedStoreFailure));
            return Horo::Result<Horo::ExclusiveFileLock>::Success(Horo::ExclusiveFileLock{});
        }

        [[nodiscard]] Horo::Result<std::uint64_t> AvailableBytes(const std::filesystem::path &) const override {
            return Horo::Result<std::uint64_t>::Success(availableBytes);
        }

        [[nodiscard]] Horo::Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            operations.push_back("write:" + path.filename().string());
            prepared.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            if (failWrite)
                return Horo::Result<void>::Failure(Horo::MakeError(kInjectedStoreFailure));
            return Horo::Result<void>::Success();
        }

        [[nodiscard]] Horo::Result<void> CopyDurable(const std::filesystem::path &, const std::filesystem::path &) override {
            return Horo::Result<void>::Failure(Horo::MakeError(kInjectedStoreFailure));
        }

        [[nodiscard]] Horo::Result<void> AtomicReplace(const std::filesystem::path &, const std::filesystem::path &path) override {
            operations.push_back("replace:" + path.filename().string());
            if (failReplace)
                return Horo::Result<void>::Failure(Horo::MakeError(kInjectedStoreFailure));
            published = prepared;
            prepared.clear();
            return Horo::Result<void>::Success();
        }

        [[nodiscard]] Horo::Result<void> RemoveDurable(const std::filesystem::path &path) override {
            operations.push_back("remove:" + path.filename().string());
            prepared.clear();
            return Horo::Result<void>::Success();
        }

        [[nodiscard]] Horo::Result<void> SyncDirectory(const std::filesystem::path &) override {
            return Horo::Result<void>::Success();
        }

        bool failLock{false};
        bool failWrite{false};
        bool failReplace{false};
        std::uint64_t availableBytes{1024 * 1024};
        std::string prepared;
        std::string published{"last-valid"};
        std::vector<std::string> operations;
    };

    void RequireRejectedWritePreservesPublishedDocument(Horo::ConfigurationFileStore &store, const Horo::ConfigurationSnapshot &snapshot,
                                                        const FaultingDurableFileSystem &files,
                                                        const std::vector<std::string> &expectedOperations) {
        REQUIRE(store.Write("settings.json", snapshot).HasError());
        REQUIRE(files.published == "last-valid");
        REQUIRE(files.prepared.empty());
        REQUIRE(files.operations == expectedOperations);
    }

    TEST_CASE("Platform Services Use Explicitly Injected Baseline Services", "[unit][foundation]") {
        RecordingFileSystem files;
        Horo::DeterministicClock clock(Horo::Duration::FromMilliseconds(10));
        FixedProcessService processes;
        Horo::StaticUserDirectories directories({
            .config = "/test/config",
            .state = "/test/state",
            .cache = "/test/cache",
            .logs = "/test/logs",
            .crash = "/test/crash",
            .temporary = "/test/tmp",
        });

        Horo::PlatformServices services(files, clock, processes, directories);

        REQUIRE((services.files.Exists("/test/project.horo")));
        REQUIRE((files.lastPath == "/test/project.horo"));
        REQUIRE((services.clock.MonotonicNow().ToMilliseconds() == 10));
        REQUIRE((services.processes.CurrentProcess().id == 42));
        REQUIRE((services.processes.EnvironmentValue("HORO_TEST") == "enabled"));
        REQUIRE((services.directories.Config() == "/test/config"));
    }

    TEST_CASE("Platform Capabilities Report Optional Service Availability", "[unit][foundation]") {
        Horo::NullFileSystem files;
        Horo::DeterministicClock clock;
        Horo::NullProcessService processes;
        Horo::StaticUserDirectories directories({});
        constexpr Horo::PlatformCapabilities capabilities{
            .supportsProcessExecution = false,
            .hasCredentialStore = false,
            .hasNativeDialogs = false,
            .hasCrashService = false,
        };

        const Horo::PlatformServices services(files, clock, processes, directories, capabilities);

        REQUIRE((!services.Capabilities().supportsProcessExecution));
        REQUIRE((!services.Capabilities().hasCredentialStore));
        REQUIRE((!services.Capabilities().hasNativeDialogs));
        REQUIRE((!services.Capabilities().hasCrashService));
    }

    TEST_CASE("Deterministic Adapters Provide Stable Clock And Paths", "[unit][foundation]") {
        Horo::DeterministicClock clock(Horo::Duration::FromMilliseconds(100));
        Horo::StaticUserDirectories directories({.temporary = "/test/tmp"});
        Horo::NullFileSystem files;

        REQUIRE((clock.MonotonicNow().ToMilliseconds() == 100));
        clock.Advance(Horo::Duration::FromMilliseconds(25));
        REQUIRE((clock.MonotonicNow().ToMilliseconds() == 125));
        REQUIRE((directories.Temporary() == "/test/tmp"));
        REQUIRE((!files.Exists("/test/missing")));
    }

    TEST_CASE("Steady Clock Is Monotonic", "[unit][foundation]") {
        const Horo::SteadyClock clock;
        const Horo::Duration first = clock.MonotonicNow();
        const Horo::Duration second = clock.MonotonicNow();
        REQUIRE((second >= first));
    }

    TEST_CASE("Native Durable Filesystem Serializes Locks And Replaces Files", "[unit][foundation]") {
        const auto root = std::filesystem::temp_directory_path() / "horo-platform-durable-test";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root);
        Horo::NativeDurableFileSystem files;
        {
            auto first = files.TryAcquireExclusive(root / "mutation.lock", "first");
            REQUIRE((first.HasValue()));
            auto second = files.TryAcquireExclusive(root / "mutation.lock", "second");
            REQUIRE((second.HasError()));
        }
        REQUIRE((files.TryAcquireExclusive(root / "mutation.lock", "after-release").HasValue()));
        const std::string text = "durable";
        std::vector<std::byte> bytes(text.size());
        std::memcpy(bytes.data(), text.data(), text.size());
        REQUIRE((files.WriteDurable(root / "prepared", bytes).HasValue()));
        REQUIRE((files.AtomicReplace(root / "prepared", root / "published").HasValue()));
        {
            std::ifstream input(root / "published", std::ios::binary);
            REQUIRE((std::string(std::istreambuf_iterator<char>(input), {}) == text));
        }
        REQUIRE((files.CopyDurable(root / "published", root / "copied").HasValue()));
        {
            std::ifstream input(root / "copied", std::ios::binary);
            REQUIRE((std::string(std::istreambuf_iterator<char>(input), {}) == text));
        }
        REQUIRE((files.AvailableBytes(root).HasValue()));
        REQUIRE((files.RemoveDurable(root / "copied").HasValue()));
        REQUIRE((files.RemoveDurable(root / "published").HasValue()));
        std::filesystem::remove_all(root, ignored);
    }

    TEST_CASE("Configuration File Store Publishes Deterministic Versioned Documents", "[unit][foundation][configuration]") {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path root = std::filesystem::temp_directory_path() / ("horo-configuration-store-" + std::to_string(stamp));
        std::filesystem::create_directories(root);
        Horo::NativeDurableFileSystem files;
        Horo::ConfigurationFileStore store{files};
        const Horo::ConfigurationSnapshot snapshot = BuildConfigurationSnapshot();
        const std::filesystem::path path = root / "settings.json";

        REQUIRE(store.Write(path, snapshot).HasValue());
        Horo::Result<std::string> first = store.Read(path);
        REQUIRE(first.HasValue());
        REQUIRE(first.Value() == snapshot.ToJson());
        REQUIRE(store.Write(path, snapshot).HasValue());
        REQUIRE(store.Read(path).Value() == first.Value());

        auto held = files.TryAcquireExclusive(root / "settings.json.lock", "competing-writer");
        REQUIRE(held.HasValue());
        REQUIRE(store.Write(path, snapshot).HasError());
        REQUIRE(store.Read(path).Value() == first.Value());

        Horo::ConfigurationLimits tiny;
        tiny.maximumDocumentBytes = 8;
        REQUIRE(store.Read(path, tiny).HasError());
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    TEST_CASE("Configuration File Store Never Publishes Partial Or Failed Writes", "[unit][foundation][configuration]") {
        const Horo::ConfigurationSnapshot snapshot = BuildConfigurationSnapshot();
        FaultingDurableFileSystem files;
        Horo::ConfigurationFileStore store{files};

        Horo::ConfigurationLimits tiny;
        tiny.maximumDocumentBytes = 8;
        REQUIRE(store.Write("settings.json", snapshot, tiny).HasError());
        REQUIRE(files.operations.empty());

        files.failWrite = true;
        RequireRejectedWritePreservesPublishedDocument(store, snapshot, files,
                                                       {"lock:settings.json.lock:horo.configuration", "write:settings.json.tmp",
                                                        "remove:settings.json.tmp"});

        files.operations.clear();
        files.failWrite = false;
        files.failReplace = true;
        RequireRejectedWritePreservesPublishedDocument(store, snapshot, files,
                                                       {"lock:settings.json.lock:horo.configuration", "write:settings.json.tmp",
                                                        "replace:settings.json", "remove:settings.json.tmp"});
    }

#if !defined(_WIN32)
    TEST_CASE("External Process Runner Keeps Arguments Out Of A Shell", "[unit][platform][process]") {
        Horo::NativeExternalProcessRunner runner;
        std::vector<Horo::ProcessOutputLine> output;
        Horo::ExternalProcessRequest request{
            .executable = "/usr/bin/printf",
            .arguments = {"%s", "literal;$(touch should-not-exist)"},
            .environment = {.base = Horo::ProcessEnvironmentBase::Replace},
            .onOutput =
                [&output](Horo::ProcessOutputLine line) {
            output.push_back(std::move(line));
        },
        };

        const auto result = runner.Run(request, {});

        REQUIRE((result.HasValue()));
        REQUIRE((result.Value().reason == Horo::ProcessTerminationReason::Exited));
        REQUIRE((result.Value().exitCode == 0));
        REQUIRE((output.size() == 1));
        REQUIRE((output.front().text == "literal;$(touch should-not-exist)"));
    }

    TEST_CASE("External Process Runner Applies Replacement Environment", "[unit][platform][process]") {
        Horo::NativeExternalProcessRunner runner;
        std::vector<std::string> output;
        Horo::ExternalProcessRequest request{
            .executable = "/usr/bin/env",
            .environment =
                {
                    .base = Horo::ProcessEnvironmentBase::Replace,
                    .set = {{"HORO_PROCESS_TEST", "deterministic"}},
                },
            .onOutput =
                [&output](Horo::ProcessOutputLine line) {
            output.push_back(std::move(line.text));
        },
        };

        const auto result = runner.Run(request, {});

        REQUIRE((result.HasValue()));
        REQUIRE((output == std::vector<std::string>{"HORO_PROCESS_TEST=deterministic"}));
    }

    TEST_CASE("External Process Runner Bounds Lines And Times Out Process Group", "[unit][platform][process]") {
        Horo::NativeExternalProcessRunner runner;
        std::vector<Horo::ProcessOutputLine> output;
        Horo::ExternalProcessRequest bounded{
            .executable = "/usr/bin/printf",
            .arguments = {"%s", std::string(128, 'x')},
            .environment = {.base = Horo::ProcessEnvironmentBase::Replace},
            .maximumLineBytes = 16,
            .onOutput =
                [&output](Horo::ProcessOutputLine line) {
            output.push_back(std::move(line));
        },
        };
        const auto boundedResult = runner.Run(bounded, {});
        REQUIRE((boundedResult.HasValue()));
        REQUIRE((output.size() == 1));
        REQUIRE((output.front().text.size() == 16));
        REQUIRE((output.front().truncated));

        Horo::ExternalProcessRequest timeout{
            .executable = "/bin/sleep",
            .arguments = {"5"},
            .environment = {.base = Horo::ProcessEnvironmentBase::Replace},
            .timeout = std::chrono::milliseconds{20},
            .gracefulTermination = std::chrono::milliseconds{20},
        };
        const auto timedOut = runner.Run(timeout, {});
        REQUIRE((timedOut.HasValue()));
        REQUIRE((timedOut.Value().reason == Horo::ProcessTerminationReason::TimedOut));
    }
#endif

    TEST_CASE("External process outcomes and bounded pipes are portable", "[unit][platform][process]") {
        Horo::NativeExternalProcessRunner runner;
        Horo::ExternalProcessRequest failure{.executable = HORO_PROCESS_TEST_CHILD, .arguments = {"exit-failure"}};
        const auto failed = runner.Run(failure, {});
        REQUIRE(failed.HasValue());
        REQUIRE(failed.Value().reason == Horo::ProcessTerminationReason::Exited);
        REQUIRE(failed.Value().exitCode == 17);

        std::vector<Horo::ProcessOutputLine> output;
        Horo::ExternalProcessRequest flood{
            .executable = HORO_PROCESS_TEST_CHILD,
            .arguments = {"flood"},
            .maximumLineBytes = 64,
            .onOutput =
                [&output](Horo::ProcessOutputLine line) {
            output.push_back(std::move(line));
        },
            .maximumOutputBytes = 128,
        };
        const auto drained = runner.Run(flood, {});
        REQUIRE(drained.HasValue());
        REQUIRE(drained.Value().reason == Horo::ProcessTerminationReason::Exited);
        REQUIRE(output.size() == 2);
        for (const auto &line : output) {
            REQUIRE(line.text.size() == 64);
            REQUIRE(line.truncated);
        }
    }

    TEST_CASE("External process cancellation and forced timeout keep their stop causes", "[unit][platform][process]") {
        Horo::NativeExternalProcessRunner runner;
        Horo::CancellationSource cancellation;
        Horo::ExternalProcessRequest sleeping{
            .executable = HORO_PROCESS_TEST_CHILD,
            .arguments = {"sleep"},
            .gracefulTermination = std::chrono::milliseconds{50},
        };
        std::thread cancelSoon{[&cancellation] {
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
            cancellation.RequestCancellation();
        }};
        const auto cancelled = runner.Run(sleeping, cancellation.Token());
        cancelSoon.join();
        REQUIRE(cancelled.HasValue());
        REQUIRE(cancelled.Value().stopCause == Horo::ProcessStopCause::Cancellation);
        REQUIRE((cancelled.Value().reason == Horo::ProcessTerminationReason::Cancelled ||
                 cancelled.Value().reason == Horo::ProcessTerminationReason::Forced));
#if !defined(_WIN32)
        REQUIRE(cancelled.Value().reason == Horo::ProcessTerminationReason::Cancelled);
#endif

        Horo::CancellationSource force;
        Horo::ExternalProcessRequest interrupted{
            .executable = HORO_PROCESS_TEST_CHILD,
            .arguments = {"stubborn"},
            .timeout = std::chrono::seconds{5},
            .gracefulTermination = std::chrono::seconds{2},
            .forceCancellation = force.Token(),
        };
        std::thread forceSoon{[&force] {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            force.RequestCancellation();
        }};
        const auto forceResult = runner.Run(interrupted, {});
        forceSoon.join();
        REQUIRE(forceResult.HasValue());
        REQUIRE(forceResult.Value().reason == Horo::ProcessTerminationReason::Forced);
        REQUIRE(forceResult.Value().stopCause == Horo::ProcessStopCause::Cancellation);

        Horo::ExternalProcessRequest stubborn{
            .executable = HORO_PROCESS_TEST_CHILD,
            .arguments = {"stubborn"},
            .timeout = std::chrono::milliseconds{500},
            .gracefulTermination = std::chrono::milliseconds{100},
            .maximumDrainDuration = std::chrono::milliseconds{100},
        };
        const auto forced = runner.Run(stubborn, {});
        REQUIRE(forced.HasValue());
        REQUIRE(forced.Value().reason == Horo::ProcessTerminationReason::Forced);
        REQUIRE(forced.Value().stopCause == Horo::ProcessStopCause::Timeout);
    }

    TEST_CASE("External process termination does not leave an owned descendant running", "[unit][platform][process]") {
        const auto marker = std::filesystem::temp_directory_path() /
                            ("horo-process-tree-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Horo::NativeExternalProcessRunner runner;
        Horo::ExternalProcessRequest tree{
            .executable = HORO_PROCESS_TEST_CHILD,
            .arguments = {"tree", marker.string()},
            .timeout = std::chrono::seconds{1},
            .gracefulTermination = std::chrono::milliseconds{100},
            .maximumDrainDuration = std::chrono::milliseconds{100},
        };
        const auto result = runner.Run(tree, {});
        REQUIRE(result.HasValue());
        REQUIRE(std::filesystem::exists(marker));
        const auto before = std::filesystem::file_size(marker);
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        REQUIRE(std::filesystem::file_size(marker) == before);
        std::error_code ignored;
        std::filesystem::remove(marker, ignored);
    }
#if !defined(_WIN32)
    TEST_CASE("External process preserves spontaneous POSIX signal outcome", "[unit][platform][process]") {
        Horo::NativeExternalProcessRunner runner;
        Horo::ExternalProcessRequest request{.executable = HORO_PROCESS_TEST_CHILD, .arguments = {"signalled"}};
        const auto result = runner.Run(request, {});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().reason == Horo::ProcessTerminationReason::Signalled);
    }
#endif
}  // namespace
