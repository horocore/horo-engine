#pragma once

/** @file AgentHistorySession.h @brief Editor-owned, non-blocking lifecycle for project conversation history. */

#include "AgentConversationStore.h"

namespace Horo {
    class JobSystem;
}

namespace Horo::Agent {
    /** @brief A committed project identity supplied by the editor project owner. */
    struct AgentHistoryProject final {
        std::filesystem::path root;
        std::string id;

        bool operator==(const AgentHistoryProject &) const = default;
    };

    enum class AgentHistorySessionPhase : std::uint8_t {
        NoProject,
        Opening,
        Settled
    };

    /** @brief Exact selected-project incarnation within this editor session. */
    struct AgentHistoryProjectRevision final {
        std::uint64_t value{};
        auto operator<=>(const AgentHistoryProjectRevision &) const noexcept = default;
    };

    /** @brief One accepted operation on the existing I/O worker. */
    struct AgentHistoryOperationId final {
        std::uint64_t value{};
        auto operator<=>(const AgentHistoryOperationId &) const noexcept = default;
    };

    /** @brief Visible assistant response and exact owner IDs, with references only. */
    struct AgentHistoryCheckpointRequest final {
        AgentConversationId conversation;
        AgentHistoryMessageId response;
        AgentHistoryMessageState state;
        std::string_view visibleText;
        std::span<const AgentHistoryReference> references;
        AgentHistoryUsage usage;
        std::uint64_t now{};
    };

    /** @brief One settled write. A successfully recorded turn also returns its durable IDs. */
    struct AgentHistoryMutationCompletion final {
        AgentHistoryProjectRevision project;
        AgentHistoryOperationId operation;
        std::optional<AgentHistoryStartedTurn> started;
        std::optional<Error> error;
        std::optional<std::string> exportedText; /**< Owned sanitized JSON from an exact export operation. */
    };

    /** @brief Borrowed editor-lane view, invalidated by the next project change, poll, or shutdown. */
    struct AgentHistorySessionView final {
        AgentHistorySessionPhase phase{AgentHistorySessionPhase::NoProject};
        std::optional<AgentHistoryAvailability> availability;
        std::span<const AgentConversation> conversations;
        AgentHistoryProjectRevision project;
    };

    /** @brief Serializes store lifetime changes and history writes on the existing editor I/O lane.
     * @details The editor lane only requests changes and publishes immutable display snapshots. No project
     * selection, SQLite access, file reads, or history recovery is part of editor startup completion.
     * The JobSystem, filesystem, and any objects borrowed by the filter outlive this session. The filter
     * is a history-only callback, with no requirement for provider or tool capabilities.
     */
    class AgentHistorySession final {
    public:
        /** @brief Construct an idle projection without accessing the filesystem.
         * @param jobs Existing editor worker owner with I/O capacity.
         * @param files Host-owned project filesystem.
         * @param filter Host-owned history text classifier/redactor.
         * @param limits Bounded retention policy for every selected project.
         */
        AgentHistorySession(const JobSystem &jobs, DurableFileSystem &files, AgentHistoryTextFilter filter,
                            const AgentHistoryLimits &limits = {});
        ~AgentHistorySession();
        AgentHistorySession(const AgentHistorySession &) = delete;
        AgentHistorySession &operator=(const AgentHistorySession &) = delete;

        /** @brief Select the committed project or clear history when the project closes.
         * @param project Exact committed project metadata or empty on close.
         * @details Immediately invalidates the old display snapshot. Superseded opens can never publish.
         * This operation is called only after the actual project transaction has committed.
         */
        void SelectProject(std::optional<AgentHistoryProject> project);

        /** @brief Advance background history lifecycle once on the editor lane without waiting. */
        void Poll();

        /** @brief Borrow the latest accepted, history-only projection on the editor lane. */
        [[nodiscard]] AgentHistorySessionView View() const noexcept;

        /** @brief Admit a user turn through the exact open project and its bounded worker queue.
         * @param project Current settled view's project revision.
         * @param request Visible user input copied into a bounded job after admission checks.
         * @return Exact operation ID or typed synchronous denial. A rejected operation does no writer work.
         */
        [[nodiscard]] Result<AgentHistoryOperationId> BeginTurn(AgentHistoryProjectRevision project,
                                                                const AgentHistoryTurnRequest &request);

        /** @brief Queue one reviewed visible dialogue pair against the exact open project and settle it atomically.
         * @param project Project revision captured with the reviewed preview.
         * @param request Visible prompt and existing conversation or new title.
         * @param answer Visible terminal response.
         * @return Exact accepted operation ID, or typed synchronous denial.
         */
        [[nodiscard]] Result<AgentHistoryOperationId> RecordCompleted(AgentHistoryProjectRevision project,
                                                                      const AgentHistoryTurnRequest &request,
                                                                      const AgentHistoryCompletedText &answer);

        /** @brief Admit a bounded response update for the exact open project.
         * @param project Current settled view's project revision.
         * @param request Exact response, visible text, and numeric/reference metadata.
         * @return Exact operation ID or typed synchronous denial. Durable settlement is observed separately.
         */
        [[nodiscard]] Result<AgentHistoryOperationId> Checkpoint(AgentHistoryProjectRevision project,
                                                                 const AgentHistoryCheckpointRequest &request);

        /** @brief Queue an exact conversation rename through the existing history text filter.
         * @param project Current settled project incarnation.
         * @param conversation Conversation within that project.
         * @param title New visible title copied only after bounded admission.
         * @return Exact accepted operation ID or typed denial.
         */
        [[nodiscard]] Result<AgentHistoryOperationId> Rename(AgentHistoryProjectRevision project, AgentConversationId conversation,
                                                             std::string_view title);

        /** @brief Queue archive or restore for one exact, non-streaming conversation.
         * @param project Current settled project incarnation.
         * @param conversation Exact conversation in that project.
         * @param archived True to archive, false to restore.
         * @return Exact accepted operation ID or typed denial.
         */
        [[nodiscard]] Result<AgentHistoryOperationId> SetArchived(AgentHistoryProjectRevision project, AgentConversationId conversation,
                                                                  bool archived);

        /** @brief Queue deletion after the editor has received explicit user confirmation.
         * @param project Current settled project incarnation.
         * @param conversation Exact conversation the user approved deleting.
         * @return Exact accepted operation ID or typed denial.
         */
        [[nodiscard]] Result<AgentHistoryOperationId> Delete(AgentHistoryProjectRevision project, AgentConversationId conversation);

        /** @brief Queue a bounded display-only projection of one accepted conversation.
         * @param project Current settled project incarnation.
         * @param conversation Exact conversation to serialize.
         * @return Exact accepted operation ID or typed denial. The receipt owns sanitized JSON without filesystem effects.
         */
        [[nodiscard]] Result<AgentHistoryOperationId> Export(AgentHistoryProjectRevision project, AgentConversationId conversation);

        /** @brief Consume the one exact write completion after Poll publishes it. Switching projects clears old completions. */
        [[nodiscard]] std::optional<AgentHistoryMutationCompletion> TakeMutation();

        /** @brief Drain accepted work and release the store at the cold editor shutdown boundary.
         * @pre Called outside scheduler callbacks, before the borrowed jobs and filesystem are destroyed.
         */
        void Shutdown() noexcept;

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Agent
