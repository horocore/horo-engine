#pragma once

/** @file GameplayTestService.h @brief Owner-lane application service for tests from a configured gameplay build. */

#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/OperationStore.h"

#include <filesystem>
#include <thread>

namespace Horo {
    class DurableFileSystem;
    class IExternalProcessRunner;
    struct ExternalProcessRequest;
}  // namespace Horo

namespace Horo::Application {
    /** @brief Host-selected CTest executable and build configuration. */
    struct GameplayTestEnvironment final {
        std::filesystem::path ctest;
        std::string configuration{"Debug"};
    };

    /** @brief The exact native input identity from a successful gameplay build. */
    struct GameplayTestBuildInput final {
        GameplayBuildRequest request;
        std::string successfulInputHash;
    };

    /** @brief Latest native status of the test operation. */
    struct GameplayTestSnapshot final {
        OperationId operation{};
        OperationState state{OperationState::Queued};
        bool cancellationRequested{};
        std::optional<Error> error;
    };

    /** @brief Runs one admitted test operation using the native output and operation stores. */
    class GameplayTestService final {
    public:
        /** @brief Borrows process, job, file, and presentation owners until shutdown on the creating lane.
         * @pre The job system has a dedicated I/O worker for process execution.
         */
        GameplayTestService(IExternalProcessRunner &processes, JobSystem &jobs, DurableFileSystem &files, OperationStore &operations,
                            BuildOutputStore &output);
        ~GameplayTestService();
        GameplayTestService(const GameplayTestService &) = delete;
        GameplayTestService &operator=(const GameplayTestService &) = delete;

        /** @brief Submits the existing configured project's native test inventory.
         * @param projectRoot Admitted host project. @param environment Explicit host toolchain selection.
         * @param stop Synchronous authorization check before admission.
         * @return Exact native operation ID or a typed admission error.
         */
        [[nodiscard]] Result<OperationId> Start(const std::filesystem::path &projectRoot, const GameplayTestEnvironment &environment,
                                                const std::function<bool()> &stop = {});
        /** @brief Runs explicitly approved tests against a published build's inputs.
         * @param build Immutable request and successful input hash retained from the approved build.
         * @param environment Host-selected CTest executable and matching configuration.
         * @param stop Synchronous authorization check before admission.
         * @return Operation ID or admission error. Changed build inputs cause a typed failure without a successful test receipt.
         */
        [[nodiscard]] Result<OperationId> StartForBuild(GameplayTestBuildInput build, const GameplayTestEnvironment &environment,
                                                        const std::function<bool()> &stop = {});
        /** @brief Reads the latest exact operation without creating work or changing its state. */
        [[nodiscard]] std::optional<GameplayTestSnapshot> Query(OperationId id) const;
        /** @brief Requests cooperative cancellation of the active exact operation. */
        [[nodiscard]] bool RequestCancel(OperationId id);
        /** @brief Projects accepted worker transitions into the shared native operation and output surfaces. */
        void Poll();
        /** @brief Cancels and joins the owned process job before the borrowed services retire. */
        void Close();

    private:
        struct Attempt;
        /** @brief Publishes a validated request and submits its process job. */
        [[nodiscard]] Result<OperationId> StartImpl(const std::filesystem::path &projectRoot, const GameplayTestEnvironment &environment,
                                                    const std::function<bool()> &stop, std::optional<GameplayTestBuildInput> build);
        /** @brief Submits an admitted request with its optional build correlation. */
        [[nodiscard]] Result<OperationId> Submit(ExternalProcessRequest request, std::optional<GameplayTestBuildInput> build);
        /** @brief Publishes one terminal native operation and final transcript record. */
        void Finish(OperationState state, std::optional<Error> error);

        const std::thread::id owner_{std::this_thread::get_id()};
        IExternalProcessRunner &processes_;
        JobSystem &jobs_;
        DurableFileSystem &files_;
        OperationStore &operations_;
        BuildOutputStore &output_;
        std::shared_ptr<Attempt> active_;
        std::optional<JobHandle> job_;
        std::optional<GameplayTestSnapshot> latest_;
        bool closed_{};
    };
}  // namespace Horo::Application
