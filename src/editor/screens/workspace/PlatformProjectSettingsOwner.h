#pragma once

/** @file PlatformProjectSettingsOwner.h @brief Workspace-private policy publication authority for shared settings drafts. */
#include "Horo/Application/PlatformServicesProjectSources.h"
#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/ProjectMutation.h"

namespace Horo::Editor::WorkspaceScreenDetail {
    /** @brief Owns accepted source witnesses and revocation, never a second UI draft or provider instance.
     * @details Owner-thread tooling only. Durable and mutation services outlive the workspace; modal callbacks hold
     * weak borrows. Close revokes every old callback before workspace retirement. Existing runtime captures remain immutable.
     */
    class PlatformProjectSettingsOwner final : public std::enable_shared_from_this<PlatformProjectSettingsOwner> {
    public:
        /** @brief Admits an exact opening and initializes its Project values in the shared authority.
         * @param sources Sealed project-open capture. @param context Actual host profile/contributions/trust.
         * @param configuration Host authority outliving every modal. @param files Host durable writer.
         * @param mutations Shared fail-fast project mutation gate. @return Revocable owner or original typed failure. */
        [[nodiscard]] static Result<std::shared_ptr<PlatformProjectSettingsOwner>> Open(
            std::shared_ptr<const Application::PlatformServicesProjectSourceSnapshot> sources,
            Application::PlatformServicesProjectValidationContext context, ConfigurationService &configuration, DurableFileSystem &files,
            ProjectMutationCoordinator &mutations);
        /** @brief Supplies weak source operations to the existing generic draft. @return Exact Project binding, without owner extension. */
        [[nodiscard]] ConfigurationSettingsPersistence Borrow();
        /** @brief Revokes all retained modal operations without touching published files or old runtime captures. */
        void Close() noexcept;

    private:
        /** @brief Retains validated models and copied host policy while borrowing host-lifetime filesystem/mutation services. */
        PlatformProjectSettingsOwner(std::shared_ptr<const Application::PlatformServicesProjectSourceSnapshot> sources,
                                     Application::PlatformServicesProjectValidationContext context, DurableFileSystem &files,
                                     ProjectMutationCoordinator &mutations);
        /** @brief Checks opening and retained reconciliation state. @return Original failure or success. */
        [[nodiscard]] Result<void> Available() const;
        /** @brief Applies the shared typed policy validator to a complete candidate. @param snapshot Shared preview.
         * @return Original shape/capability/trust error or success. */
        [[nodiscard]] Result<void> Validate(const ConfigurationSnapshot &snapshot) const;
        /** @brief Publishes under fail-fast mutation and exact-source leases. @param snapshot Prepared configuration candidate.
         * @return Original precommit failure, retained reconciliation error or success. */
        [[nodiscard]] Result<void> Persist(const ConfigurationSnapshot &snapshot);

        std::shared_ptr<const Application::PlatformServicesProjectSourceSnapshot> sources_;
        Application::PlatformServicesProjectValidationContext context_;
        DurableFileSystem &files_;
        ProjectMutationCoordinator &mutations_;
        std::optional<Assets::AssetSourcePublication> publication_;
        bool closed_{};
    };
}  // namespace Horo::Editor::WorkspaceScreenDetail
