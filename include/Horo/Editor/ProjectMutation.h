#pragma once

/** @file ProjectMutation.h @brief Shared project mutation serialization contract. */

#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Foundation/Platform.h"

#include <filesystem>
#include <memory>
#include <string>

namespace Horo::Editor {
    /** @brief Describes existing project-mutation errors without acquiring locks or changing project state.
     * @return Owned inert metadata containing process-lifetime immutable descriptor pointers.
     */
    [[nodiscard]] ModuleErrorDomainDescriptor ProjectMutationErrorDomain();

    /** @brief Names the subsystem requesting exclusive authored-project mutation authority. */
    enum class ProjectMutationOwner : std::uint8_t {
        Migration,
        Save,
        Autosave,
        Package,
        Asset,
        Cli,
        Mcp
    };

    /** @brief Identifies one immediate project mutation admission request. */
    struct ProjectMutationRequest {
        std::filesystem::path projectRoot;
        ProjectMutationOwner owner{ProjectMutationOwner::Migration};
        std::string operationId;
    };

    /** @brief Move-only proof that one process and host operation owns project mutation authority. */
    class ProjectMutationLease {  // NOSONAR(cpp:S3624) Pimpl pattern: destructor defined in .cpp to complete State
    public:
        struct State;
        ProjectMutationLease(const ProjectMutationLease &) = delete;
        ProjectMutationLease &operator=(const ProjectMutationLease &) = delete;
        ProjectMutationLease(ProjectMutationLease &&) noexcept;
        ProjectMutationLease &operator=(ProjectMutationLease &&) noexcept;
        ~ProjectMutationLease();

    private:
        friend class ProjectMutationCoordinator;
        explicit ProjectMutationLease(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };

    /** @brief Serializes in-process and cross-process writers through one project-local lock. */
    class ProjectMutationCoordinator {
    public:
        struct State;
        /** @brief Constructs a host-owned coordinator. @param files Durable native filesystem service. */
        explicit ProjectMutationCoordinator(DurableFileSystem &files);
        ~ProjectMutationCoordinator();
        ProjectMutationCoordinator(const ProjectMutationCoordinator &) = delete;
        ProjectMutationCoordinator &operator=(const ProjectMutationCoordinator &) = delete;
        ProjectMutationCoordinator(ProjectMutationCoordinator &&) = delete;
        ProjectMutationCoordinator &operator=(ProjectMutationCoordinator &&) = delete;

        /**
         * @brief Immediately attempts to acquire exclusive mutation authority.
         * @param request Project root and diagnostic operation identity.
         * @return Move-only lease, or a typed busy/filesystem failure without waiting.
         */
        [[nodiscard]] Result<ProjectMutationLease> TryAcquire(const ProjectMutationRequest &request) const;

    private:
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Editor
