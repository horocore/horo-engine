#pragma once

/**
 * @file GameplayBuildService.h
 * @brief Asynchronous cache-aware native gameplay module build capability.
 */

#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/OperationStore.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace Horo {
    class DurableFileSystem;
    class IExternalProcessRunner;
}  // namespace Horo

namespace Horo::Application {
    using GameplayBuildSessionId = std::uint64_t;

    /** @brief Observable lifecycle of one project gameplay build request. */
    enum class GameplayBuildState : std::uint8_t {
        Queued,
        AcquiringLock,
        WaitingForExternalBuild,
        Configuring,
        Building,
        Validating,
        Succeeded,
        Failed,
        Cancelled,
        TimedOut,
    };

    /** @brief Per-phase bounded timeout policy. */
    struct GameplayBuildTimeoutPolicy {
        std::chrono::milliseconds configure{std::chrono::minutes{5}};
        std::chrono::milliseconds build{std::chrono::minutes{15}};
        std::chrono::milliseconds externalWait{std::chrono::minutes{20}};
        std::chrono::milliseconds gracefulTermination{std::chrono::seconds{2}};
    };

    /** @brief Explicit host-selected CMake and SDK environment. */
    struct GameplayBuildEnvironment {
        std::filesystem::path gameplaySdkPackage;
        std::string cmakeExecutable{"cmake"};
        std::string configuration{"Debug"};
        std::optional<std::filesystem::path> cxxCompiler;
        std::optional<std::string> generator;
        std::optional<std::string> generatorPlatform;
        std::optional<std::string> generatorToolset;
        std::optional<std::filesystem::path> toolchainFile;
    };

    /** @brief Immutable inputs for one build request. */
    struct GameplayBuildRequest {
        std::filesystem::path projectRoot;
        GameplayBuildEnvironment environment;
        GameplayBuildTimeoutPolicy timeouts;
    };

    /** @brief Owned thread-safe projection of one build session. */
    struct GameplayBuildSnapshot {
        GameplayBuildSessionId id{};
        std::chrono::steady_clock::time_point startedAt{};
        GameplayBuildState state{GameplayBuildState::Queued};
        std::string phase;
        std::optional<float> progress;
        std::string activeInputHash;
        std::string desiredInputHash;
        std::optional<std::string> pendingInputHash;
        std::uint64_t coalescedRequestCount{};
        bool newerInputsPending{false};
        bool cancellationRequested{false};
        std::string externalLockOwner;
        std::optional<std::chrono::steady_clock::time_point> timeoutDeadline;
        std::optional<OperationId> operationId;
        std::optional<Error> error;
    };

    /** @brief Project-scoped asynchronous native gameplay build authority. */
    class GameplayBuildService final {
    public:
        struct State;

        /**
         * @brief Creates a service borrowing process, worker, filesystem and optional observation authorities.
         */
        GameplayBuildService(IExternalProcessRunner &processes, JobSystem &jobs, DurableFileSystem &files,
                             BuildOutputStore *output = nullptr, OperationStore *operations = nullptr);
        ~GameplayBuildService();
        GameplayBuildService(const GameplayBuildService &) = delete;
        GameplayBuildService &operator=(const GameplayBuildService &) = delete;

        /** @brief Starts, joins, or coalesces a project build. */
        [[nodiscard]] Result<GameplayBuildSessionId> Start(GameplayBuildRequest request) const;
        /** @brief Returns the latest session snapshot. */
        [[nodiscard]] std::optional<GameplayBuildSnapshot> Query(GameplayBuildSessionId id) const;
        /**
         * @brief Returns the active session for one project, if any.
         * @param projectRoot Project root used when starting the build.
         * @return An owned snapshot; terminal sessions are excluded.
         */
        [[nodiscard]] std::optional<GameplayBuildSnapshot> QueryActiveProject(const std::filesystem::path &projectRoot) const;
        /** @brief Requests cooperative cancellation for an active session. */
        [[nodiscard]] bool RequestCancel(GameplayBuildSessionId id) const;
        /** @brief Reports whether the last validated build matches current project inputs. */
        [[nodiscard]] bool IsUpToDate(const GameplayBuildRequest &request) const;
        /** @brief Cancels and joins all service-owned work. */
        void Shutdown() const noexcept;

    private:
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Application
