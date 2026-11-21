#pragma once

/** @file ProjectLanguageWorkspace.h @brief Project-level routing and ownership of approved language servers. */

#include "LspProcess.h"

#include <filesystem>
#include <memory>
#include <span>

namespace Horo::Editor {
    class SourceFileOpenService;
}

namespace Horo::Editor::Language {
    /** @brief Host-approved launch configuration and case-insensitive path suffix routing. */
    struct ProjectLanguageProvider final {
        std::string id;
        std::string languageId;
        std::vector<std::string> extensions;
        std::filesystem::path executable;
        std::vector<std::string> arguments;
    };

    /** @brief Correlation handle scoped to a provider within one project workspace. */
    struct ProjectLanguageRequest final {
        std::size_t provider{};
        SourceLanguageRequestId request;
        std::weak_ptr<const std::byte> workspace;
    };

    /** @brief Owns one lazy, shared language process per configured provider in a project.
     * @details The runner and source owner outlive this workspace. Methods run on the workspace owner lane.
     * Provider configuration is supplied by the trusted host. Project contents never choose executable paths.
     * Poll after Shutdown until Finished before handing control to another workspace. Destruction
     * cancels and joins any remaining worker during host teardown.
     */
    class ProjectLanguageWorkspace final {
    public:
        using Clock = LspProcess::Clock;

        /** @brief Validates provider ownership and constructs the project URI without starting a process. */
        [[nodiscard]] static Result<std::unique_ptr<ProjectLanguageWorkspace>> Create(IExternalProcessRunner &runner,
                                                                                      SourceFileOpenService &sources,
                                                                                      std::span<const ProjectLanguageProvider> providers);
        /** @brief Releases every process owned by this project. */
        ~ProjectLanguageWorkspace();

        ProjectLanguageWorkspace(const ProjectLanguageWorkspace &) = delete;
        ProjectLanguageWorkspace &operator=(const ProjectLanguageWorkspace &) = delete;

        /** @brief Reconciles open snapshots and pumps one bounded output quantum per provider.
         * @param documents Current open source snapshots. @param now Monotonic observation time.
         * @return At most one typed result per configured provider. A failed server does not stop other providers.
         */
        [[nodiscard]] std::vector<LanguageResult> Poll(std::span<const SourceDocumentSnapshot> documents, Clock::time_point now);

        /** @brief Routes a query by its exact synchronized source and host language selection. */
        [[nodiscard]] Result<ProjectLanguageRequest> Request(const SourceDocumentSnapshot &source, SourceLanguageRequestKind kind,
                                                             LspSourceQuery query = {});
        /** @brief Cancels a request on its original provider and transport incarnation. */
        [[nodiscard]] Result<void> Cancel(ProjectLanguageRequest request);

        /** @brief Requests a fresh incarnation after bounded retirement of the current process. */
        [[nodiscard]] bool Restart(std::string_view providerId, Clock::time_point now);
        /** @brief Returns the last launch or protocol error for a configured provider. */
        [[nodiscard]] const Error *ProviderError(std::string_view providerId) const noexcept;

        /** @brief Stops admitting source work and initiates graceful retirement on every provider. */
        void Shutdown(Clock::time_point now);
        /** @brief Reports whether every owned worker has returned after Shutdown. */
        [[nodiscard]] bool Finished() const noexcept;

    private:
        struct Provider;

        /** @brief Constructs a workspace for an already validated project URI. */
        ProjectLanguageWorkspace(IExternalProcessRunner &runner, const SourceDocumentService &documents, std::filesystem::path root,
                                 std::string uri);
        /** @brief Resolves a configured provider index from the snapshot's canonical path suffix. */
        [[nodiscard]] std::optional<std::size_t> Select(const SourceDocumentSnapshot &source) const;
        /** @brief Starts an admitted provider using only the copied trusted launch settings. */
        void Start(Provider &provider, Clock::time_point now);
        /** @brief Reconciles admitted documents assigned to this provider against open snapshots. */
        void Sync(Provider &provider, std::span<const SourceDocumentSnapshot> documents, std::size_t providerIndex);
        /** @brief Pumps current process output and releases a finished worker. */
        void Pump(Provider &provider, std::vector<LanguageResult> &results, Clock::time_point now);
        /** @brief Stops one process without waiting on the editor lane. */
        void Stop(Provider &provider, Clock::time_point now);

        IExternalProcessRunner &runner_;
        const SourceDocumentService &documents_;
        std::filesystem::path root_;
        std::string uri_;
        std::vector<std::unique_ptr<Provider>> providers_;
        std::shared_ptr<const std::byte> identity_ = std::make_shared<const std::byte>();
        bool shuttingDown_{};
    };
}  // namespace Horo::Editor::Language
