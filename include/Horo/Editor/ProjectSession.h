#pragma once

/** @file ProjectSession.h @brief Validated project-session candidate and activation reservation contracts. */

#include "Horo/Application/PlatformServicesProjectSources.h"
#include "Horo/Application/ProjectSurfaceSettings.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Horo::Editor {
    /** @brief Generation-safe identity of one project-open attempt. */
    struct ProjectOpenOperationId {
        std::uint64_t value{};
        [[nodiscard]] bool operator==(const ProjectOpenOperationId &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one unpublished project session candidate. */
    struct ProjectSessionCandidateId {
        std::uint64_t value{};
        [[nodiscard]] bool operator==(const ProjectSessionCandidateId &) const noexcept = default;
    };

    /** @brief Immutable project data verified before an editor workspace may be constructed.
     * @details Platform models and all six source witnesses are retained by shared const ownership.
     * Callers retaining models beyond the activation lease must copy that shared ownership.
     */
    struct ProjectSessionCandidate {
        ProjectSessionCandidateId id;
        ProjectOpenOperationId sourceOperation;
        std::filesystem::path projectRoot;
        std::string projectName;
        std::string rendererBackend;
        std::vector<std::string> derivedStateRevisions;
        Application::ProjectSurfaceFileSnapshot surfaces; /**< Exact validated catalog retained through activation. */
        std::shared_ptr<const Application::PlatformServicesProjectSourceSnapshot>
            platformServices; /**< Complete source/configuration generation, fenced through activation. */
    };

    /** @brief Reservation over one ready session; destruction rolls an uncommitted reservation back to ready. */
    class ProjectSessionActivationLease {
    public:
        ProjectSessionActivationLease() = default;
        ProjectSessionActivationLease(ProjectSessionActivationLease &&other) noexcept;
        ProjectSessionActivationLease &operator=(ProjectSessionActivationLease &&other) noexcept;
        ProjectSessionActivationLease(const ProjectSessionActivationLease &) = delete;
        ProjectSessionActivationLease &operator=(const ProjectSessionActivationLease &) = delete;
        ~ProjectSessionActivationLease();

        /** @brief Returns the reserved immutable candidate. */
        [[nodiscard]] const ProjectSessionCandidate &Candidate() const noexcept;
        /** @brief Consumes the candidate after workspace construction succeeds. */
        [[nodiscard]] Result<void> Commit();

    private:
        struct State;
        friend class ProjectOpenService;
        ProjectSessionActivationLease(std::shared_ptr<State> state, ProjectSessionCandidateId id) noexcept;
        void Release() noexcept;
        std::shared_ptr<State> state_;
        ProjectSessionCandidateId id_{};
        bool committed_{};
    };
}  // namespace Horo::Editor
