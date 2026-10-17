#pragma once
/** @file NavigationProjectSettingsSession.h
 * @brief Persistent project-host navigation document/workflow composition; UI owns drafts only.
 */
#include "Horo/Application/NavigationProjectSettingsStore.h"
#include "Horo/Editor/NavigationSettingsWorkflow.h"
#include "Horo/Editor/ProjectMutation.h"

#include <thread>

namespace Horo::Editor {
    class NavigationProjectSettingsSession;

    /** @brief Revocable owner-thread UI borrow; never retains the project session or redirects to a later opening. */
    class NavigationProjectSettingsSessionBorrow final {
    public:
        /** @brief Creates an inert handle that resolves no owner. */
        NavigationProjectSettingsSessionBorrow() noexcept = default;
        /** @brief Resolves the captured opening without allocation or filesystem access.
         * @return Borrowed live owner, or null after Close/destruction or on another thread.
         * @pre Use synchronously on the owner thread, without reentrant session closure; never retain across an event-loop boundary.
         */
        [[nodiscard]] NavigationProjectSettingsSession *Resolve() const noexcept;

    private:
        friend class NavigationProjectSettingsSession;

        struct Anchor final {
            NavigationProjectSettingsSession *owner{};
            std::thread::id thread;
        };

        explicit NavigationProjectSettingsSessionBorrow(std::weak_ptr<Anchor> anchor) noexcept : anchor_(std::move(anchor)) {}

        std::weak_ptr<Anchor> anchor_;
    };

    /** @brief Per-file save outcome; Save All is deliberately not an atomic multi-file replacement. */
    struct NavigationDocumentSaveOutcome final {
        Assets::AssetId asset;
        std::optional<Error> error;
        bool durable{};
    };

    /** @brief Actual project source owner used by Settings and headless tooling, independent of surface lifetime. */
    class NavigationProjectSettingsSession final {
    public:
        /** @brief Loads all registered project navigation/Scene inputs from real bounded physically-contained source bytes.
         * @param root Explicit authorized project root. @param registry Host registry. @param files Host durable filesystem.
         * @param mutations Shared project writer gate. @param generation Fresh host-issued opening generation.
         * @param activeScene Optional observational Scene port borrow; matching asset retains its weak canonical root/history handle.
         * The port is inspected during Open only; later publication still requires explicit mutable session commands.
         * @param targets Explicit immutable target/provider metadata; absence remains unknown.
         * @return Persistent session or original parse/containment/availability failure, never a fabricated empty inventory.
         */
        [[nodiscard]] static Result<std::unique_ptr<NavigationProjectSettingsSession>> Open(
            std::filesystem::path root, const Assets::AssetRegistry &registry, DurableFileSystem &files,
            ProjectMutationCoordinator &mutations, std::uint64_t generation, const SceneDocumentTransactions *activeScene = nullptr,
            std::span<const Application::NavigationTargetProviderEvidence> targets = {});
        ~NavigationProjectSettingsSession();
        /** @brief Captures this exact opening for UI presentation. @return Non-owning revocable owner-thread handle. */
        [[nodiscard]] NavigationProjectSettingsSessionBorrow Borrow() const noexcept;
        NavigationProjectSettingsSession(const NavigationProjectSettingsSession &) = delete;
        NavigationProjectSettingsSession &operator=(const NavigationProjectSettingsSession &) = delete;
        /** @brief Borrows the cached owner-thread definition projection without allocation or filesystem access.
         * @return Complete revision/session-qualified snapshots or lifecycle failure.
         * @pre The span is used only until the next Commit, Replay or Close; copy an individual snapshot for an immutable lease.
         * @details Successful commit/replay refreshes existing slots in place; Close invalidates all borrowed views.
         */
        [[nodiscard]] Result<std::span<const NavigationDefinitionSnapshot>> Definitions() const;
        /** @brief Returns optional accepted project policy. @return Absence remains explicit until user creation. */
        [[nodiscard]] std::shared_ptr<const Application::NavigationProjectSettings> Policy() const;
        /** @brief Validates and saves policy independently of asset/Scene semantics.
         * @param input Complete explicitly authored successor. @return Actual per-file publication receipt.
         */
        [[nodiscard]] Result<Application::NavigationSettingsPublication> SavePolicy(Application::NavigationProjectSettingsInput input);
        /** @brief Returns host-captured target/provider metadata; empty means unknown, never supported by inference. */
        [[nodiscard]] std::span<const Application::NavigationTargetProviderEvidence> TargetEvidence() const noexcept;
        /** @brief Checks actual file freshness and captures exhaustive reference review.
         * @param definition Asset identity. @param edits Typed gesture. @return Immutable review or typed diagnostic.
         */
        [[nodiscard]] Result<NavigationSettingsReview> Preview(Assets::AssetId definition,
                                                               std::span<const NavigationDefinitionEdit> edits) const;
        /** @brief Rechecks source freshness and commits the exact confirmed review through all canonical owners.
         * @param review Previously displayed full impact report. @return Success or zero-mutation failure.
         */
        [[nodiscard]] Result<void> Commit(const NavigationSettingsReview &review);
        /** @brief Replays one complete coordinated group. @param redo Direction. @return Atomic in-memory outcome. */
        [[nodiscard]] Result<void> Replay(bool redo);
        /** @brief Publishes dirty documents individually through Assets' exact identity/atomic source protocol.
         * @return One outcome per dirty document; a fatal owner/I/O failure may follow earlier durable files.
         * Failures never imply all-file rollback or clear an unacknowledged dirty state.
         */
        [[nodiscard]] Result<std::vector<NavigationDocumentSaveOutcome>> SaveDocuments();
        /** @brief Requests persistent host presentation without lending mutable authoring state.
         * @param asset Definition selected by the user; the host validates its registered source before opening.
         */
        void RequestOpenDefinition(Assets::AssetId asset) noexcept;
        /** @brief Consumes one owner-lane presentation request. @return Requested asset or explicit absence. */
        [[nodiscard]] std::optional<Assets::AssetId> TakeOpenDefinitionRequest() noexcept;
        /** @brief Includes definition and inactive Scene documents in the host's leave guard. @return True for unsaved authored state. */
        [[nodiscard]] bool IsDirty() const;
        /** @brief Closes admission after explicit host save/discard policy; outstanding weak review handles expire. */
        void Close() noexcept;

    private:
        NavigationProjectSettingsSession();
        struct State;
        std::unique_ptr<State> state_;
        std::shared_ptr<NavigationProjectSettingsSessionBorrow::Anchor> borrow_;
        /** @brief Loads the host-enumerated canonical authoring sources with bounded total owner count. */
        [[nodiscard]] Result<void> LoadRegisteredSources(std::uint64_t generation, const SceneDocumentTransactions *activeScene);
        /** @brief Refreshes preallocated slots only after owner admission/successful mutation, without allocation or callbacks. */
        void RefreshDefinitionProjection() noexcept;
        /** @brief Loads optional policy and checks its exact registered definition binding. */
        [[nodiscard]] Result<void> LoadPolicy();
        /** @brief Requires exact captured physical source identities and registry revision before authoring commits. */
        [[nodiscard]] Result<void> Fresh() const;
    };
}  // namespace Horo::Editor
