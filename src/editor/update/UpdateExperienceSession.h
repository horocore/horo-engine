#pragma once

#include "Horo/Foundation/JobSystem.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Editor {
    enum class EditorUpdateChannelKind : std::uint8_t {
        Stable,
        Preview,
        Nightly,
        Enterprise,
        Offline
    };

    struct EditorUpdateChannel final {
        EditorUpdateChannelKind kind{EditorUpdateChannelKind::Stable};
        std::string sourceId;
        bool operator==(const EditorUpdateChannel &) const noexcept = default;
    };

    /** @brief A verified update offer and the presentation facts supplied by the host adapter. */
    struct EditorUpdateOffer final {
        std::string version;
        std::string releaseNotes;
        std::vector<std::string> compatibilityImpacts;
        bool requiresRestart{true};
        std::uint64_t id{}; /**< Host-generated identity binding actions to one verified package. */
    };

    enum class EditorUpdatePhase : std::uint8_t {
        Idle,
        Checking,
        UpToDate,
        Available,
        Downloading,
        Verifying,
        Staged,
        RestartRequired,
        Activating,
        RollbackPending,
        Active,
        Failed,
        RolledBack,
    };

    struct EditorUpdateSnapshot final {
        EditorUpdatePhase phase{EditorUpdatePhase::Idle};
        EditorUpdateChannel channel{};
        bool automaticDownload{};
        bool installOnExit{};
        bool offline{};
        bool canCancel{};
        std::optional<EditorUpdateOffer> offer;
        std::uint64_t transferredBytes{};
        std::uint64_t totalBytes{};
        std::string diagnostic;
    };

    /** @brief Worker-only update operations composed at the editor host boundary. */
    class IEditorUpdateBackend {
    public:
        virtual ~IEditorUpdateBackend() = default;
        [[nodiscard]] virtual Result<std::optional<EditorUpdateOffer>> Check(const EditorUpdateChannel &channel,
                                                                             CancellationToken cancellation) = 0;
        [[nodiscard]] virtual Result<void> Prepare(
            const EditorUpdateOffer &offer, CancellationToken cancellation,
            const std::function<void(EditorUpdatePhase, std::uint64_t, std::uint64_t)> &progress) = 0;
        [[nodiscard]] virtual Result<void> Activate(CancellationToken cancellation) = 0;
        [[nodiscard]] virtual Result<void> Rollback(CancellationToken cancellation) = 0;
    };

    /**
     * @brief Host-owned update workflow; closing its settings view does not cancel or destroy accepted jobs.
     * @note Invoke actions and Poll on the editor owner thread. Shutdown cancels and joins every owned worker.
     */
    class UpdateExperienceSession final {
    public:
        UpdateExperienceSession(JobSystem &jobs, IEditorUpdateBackend &backend);
        ~UpdateExperienceSession();
        UpdateExperienceSession(const UpdateExperienceSession &) = delete;
        UpdateExperienceSession &operator=(const UpdateExperienceSession &) = delete;

        [[nodiscard]] const EditorUpdateSnapshot &Snapshot() const noexcept;
        [[nodiscard]] bool SetChannel(EditorUpdateChannel channel, bool confirmed = false);
        void SetAutomaticDownload(bool enabled) noexcept;
        void SetInstallOnExit(bool enabled) noexcept;
        [[nodiscard]] bool CheckNow();
        [[nodiscard]] bool Download();
        [[nodiscard]] bool RestartNow(bool confirmed);
        /** @brief Requests a staged helper handoff during orderly host exit when the user enabled that policy. */
        [[nodiscard]] Result<bool> ActivateOnExit();
        [[nodiscard]] bool Rollback(bool confirmed);
        [[nodiscard]] bool Cancel();
        /** @brief Applies a verified installed outcome reported by the host after helper completion. */
        [[nodiscard]] bool ReportVerifiedHostOutcome(EditorUpdatePhase outcome);
        void Poll();
        void Shutdown() noexcept;

    private:
        struct Completion;
        enum class Operation : std::uint8_t {
            None,
            Check,
            Prepare,
            Activate,
            Rollback
        };
        [[nodiscard]] bool Start(Operation operation);
        [[nodiscard]] static Result<void> RunOperation(IEditorUpdateBackend &backend, const std::shared_ptr<Completion> &completion,
                                                       Operation operation, const EditorUpdateChannel &channel,
                                                       const std::optional<EditorUpdateOffer> &offer, CancellationToken cancellation);
        JobSystem &jobs_;
        IEditorUpdateBackend &backend_;
        EditorUpdateSnapshot snapshot_;
        std::shared_ptr<Completion> completion_;
        CancellationSource cancellation_;
        std::optional<JobHandle> job_;
        Operation operation_{Operation::None};
        bool shutdown_{};
    };
}  // namespace Horo::Editor
