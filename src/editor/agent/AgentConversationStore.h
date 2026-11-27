#pragma once

/** @file AgentConversationStore.h @brief Project-local, bounded editor agent history. */

#include "Horo/Foundation/Platform.h"
#include "Horo/Security/AgentPolicy.h"

#include <functional>
#include <memory>
#include <span>

namespace Horo::Agent {
    struct AgentConversationId final {
        std::uint64_t value{};
        auto operator<=>(const AgentConversationId &) const noexcept = default;
    };

    struct AgentHistoryMessageId final {
        std::uint64_t value{};
        auto operator<=>(const AgentHistoryMessageId &) const noexcept = default;
    };

    enum class AgentHistoryMessageState : std::uint8_t {
        Streaming,
        Complete,
        Incomplete,
        Cancelled,
        Failed
    };
    enum class AgentHistoryRole : std::uint8_t {
        User,
        Assistant
    };
    enum class AgentHistoryReferenceKind : std::uint8_t {
        Asset,
        Document,
        ToolCall,
        BuildOutput
    };
    enum class AgentHistoryTextKind : std::uint8_t {
        Title,
        UserMessage,
        AssistantMessage
    };

    /** @brief An opaque host-issued reference, with no path, argument, or attachment payload. */
    struct AgentHistoryReference final {
        AgentHistoryReferenceKind kind{};
        std::uint64_t identity{};
        std::uint64_t revision{};
    };

    /** @brief Counts retained for display without raw provider metadata. */
    struct AgentHistoryUsage final {
        static constexpr std::uint32_t MaximumTokenCount = 1'000'000;
        std::uint32_t inputTokens{};
        std::uint32_t outputTokens{};

        /** @brief Admits each reported count independently and omits absent or oversized counts. */
        [[nodiscard]] static AgentHistoryUsage FromReported(std::optional<std::uint64_t> input,
                                                            std::optional<std::uint64_t> output) noexcept;
    };

    /** @brief Visible prompt and optional existing conversation. A new conversation needs a filtered title. */
    struct AgentHistoryTurnRequest final {
        std::optional<AgentConversationId> conversation;
        std::string_view title;
        std::string_view userText;
        std::span<const AgentHistoryReference> references;
        std::uint64_t now{};
    };

    /** @brief Exact durable conversation and response IDs assigned by the store. */
    struct AgentHistoryStartedTurn final {
        AgentConversationId conversation;
        AgentHistoryMessageId response;
    };

    /** @brief Visible terminal assistant text and optional bounded numeric usage. */
    struct AgentHistoryCompletedText final {
        std::string_view assistant;
        AgentHistoryUsage usage;
        std::span<const AgentHistoryReference> references{};
    };

    /** @brief Sanitized history projection. This type grants no tool or provider authority. */
    struct AgentHistoryMessage final {
        AgentHistoryMessageId id;
        AgentHistoryRole role{};
        AgentHistoryMessageState state{};
        std::string text;
        std::vector<AgentHistoryReference> references;
        AgentHistoryUsage usage;
    };

    /** @brief Read-only projection of one project-local conversation. */
    struct AgentConversation final {
        AgentConversationId id;
        std::string title;
        std::uint64_t updatedUnixSeconds{};
        bool archived{};
        std::vector<AgentHistoryMessage> messages;
    };

    /** @brief Explicit host classification and redaction output. A denied item uses no admitted text. */
    struct AgentHistoryTextDecision final {
        Security::AgentDataClass classification{};
        std::optional<std::string> admittedText;
    };

    /** @brief The host owns classification and redaction. The callback runs synchronously on the store's owner lane. */
    using AgentHistoryTextFilter = std::function<AgentHistoryTextDecision(AgentHistoryTextKind, std::string_view)>;

    struct AgentHistoryLimits final {
        static constexpr std::size_t MaximumTitleBytes = 256;
        std::size_t conversations{32};
        std::size_t messagesPerConversation{128};
        std::size_t textBytesPerMessage{16U << 10U};
        std::size_t referencesPerMessage{8};
        std::size_t snapshotBytes{1U << 20U};
    };

    enum class AgentHistoryAvailability : std::uint8_t {
        Ready,
        Recovered,
        DurabilityUnconfirmed,
        Unavailable,
        Corrupt,
        UnsupportedVersion,
        ForeignProject
    };

    namespace AgentHistoryErrors {
        extern const ErrorCodeDescriptor Invalid;
        extern const ErrorCodeDescriptor Capacity;
        extern const ErrorCodeDescriptor Stale;
        extern const ErrorCodeDescriptor RejectedText;
        extern const ErrorCodeDescriptor Unavailable;
        extern const ErrorCodeDescriptor Busy;
    }  // namespace AgentHistoryErrors

    /** @brief An editor worker-owned, process-exclusive history service for one authorized project.
     * @details Reads and writes may block. Open on an editor worker outside startup-critical work. History failures
     * return an inspectable empty store and leave the original file available for recovery. Borrowed file primitives
     * and the host's text filter must remain valid through the store lifetime. Access is serialized by the host.
     */
    class AgentConversationStore final {
    public:
        /** @brief Open an exact project store and recover interrupted responses before publishing them.
         * @param files Filesystem authority borrowed for the store lifetime.
         * @param projectRoot Existing authorized project root.
         * @param projectId Stable project identity read from the active project's metadata.
         * @param textFilter Required history-only redaction and classification boundary.
         * @param limits Bounded retention and snapshot limits.
         * @return Ready history or an inspectable unavailable store with zero published records.
         */
        [[nodiscard]] static AgentConversationStore Open(DurableFileSystem &files, const std::filesystem::path &projectRoot,
                                                         std::string_view projectId, AgentHistoryTextFilter textFilter,
                                                         const AgentHistoryLimits &limits = {});
        ~AgentConversationStore();
        AgentConversationStore(AgentConversationStore &&) noexcept;
        AgentConversationStore &operator=(AgentConversationStore &&) noexcept;

        /** @brief Read the availability or recovery status. */
        [[nodiscard]] AgentHistoryAvailability Availability() const noexcept;
        /** @brief Borrow the current sanitized snapshot until the next successful mutation. */
        [[nodiscard]] std::span<const AgentConversation> Conversations() const noexcept;
        /** @brief Share the immutable, already allocated committed projection across the worker boundary. */
        [[nodiscard]] std::shared_ptr<const std::vector<AgentConversation>> ConversationSnapshot() const noexcept;

        /** @brief Create a conversation with a host-reviewed title. */
        [[nodiscard]] Result<AgentConversationId> Create(std::string_view title, std::uint64_t now);
        /** @brief Change the title already reviewed by the host's history filter. */
        [[nodiscard]] Result<void> Rename(AgentConversationId conversation, std::string_view title);
        /** @brief Update archive state. Active streaming turns cannot be archived. */
        [[nodiscard]] Result<void> SetArchived(AgentConversationId conversation, bool archived);
        /** @brief Remove a conversation after host confirmation. */
        [[nodiscard]] Result<void> Delete(AgentConversationId conversation);
        /** @brief Archive conversations idle before the explicit threshold, excluding active turns. */
        [[nodiscard]] Result<void> ArchiveBefore(std::uint64_t idleBeforeUnixSeconds);
        /** @brief Export one sanitized conversation to a caller-owned string without filesystem effects. */
        [[nodiscard]] Result<std::string> Export(AgentConversationId conversation) const;

        /** @brief Atomically append the user message and one new streaming assistant response.
         * @param conversation Exact open conversation.
         * @param userText Visible user text before the history filter.
         * @param references Explicit host-issued references presented with the user message.
         * @param now Host epoch seconds.
         * @return Stable assistant message identity required by Checkpoint.
         */
        [[nodiscard]] Result<AgentHistoryMessageId> BeginTurn(AgentConversationId conversation, std::string_view userText,
                                                              std::span<const AgentHistoryReference> references, std::uint64_t now);
        /** @brief Start a turn in an existing conversation or create its conversation in the same durable commit.
         * @param request Visible, host-filtered input and exact conversation selection or new title.
         * @return Durable conversation and response IDs, or a typed failure without a partial conversation.
         */
        [[nodiscard]] Result<AgentHistoryStartedTurn> BeginTurn(const AgentHistoryTurnRequest &request);
        /** @brief Publish an explicitly reviewed prompt/response pair in one durable commit.
         * @param request Visible user input and exact existing conversation or new title.
         * @param answer Visible terminal response. Both strings pass through the store's admission filter.
         * @return Durable conversation and message IDs or a typed denial without a partial turn.
         */
        [[nodiscard]] Result<AgentHistoryStartedTurn> RecordCompleted(const AgentHistoryTurnRequest &request,
                                                                      const AgentHistoryCompletedText &answer);
        /** @brief Replace the visible response checkpoint or settle the active response.
         * @param conversation Exact owning conversation.
         * @param response Exact streaming assistant message returned by BeginTurn.
         * @param state Streaming or explicit terminal outcome.
         * @param visibleText Complete visible response preview before history filtering.
         * @details An incomplete, cancelled or failed settlement without content or metadata retains the last approved checkpoint.
         * @param references Explicit host-issued safe references for this response.
         * @param usage Bounded aggregate token counts.
         * @param now Host epoch seconds.
         * @return Success or typed stale, rejected-text, capacity or storage error.
         */
        [[nodiscard]] Result<void> Checkpoint(AgentConversationId conversation, AgentHistoryMessageId response,
                                              AgentHistoryMessageState state, std::string_view visibleText,
                                              std::span<const AgentHistoryReference> references, AgentHistoryUsage usage,
                                              std::uint64_t now);

    private:
        struct State;
        explicit AgentConversationStore(std::unique_ptr<State> state) noexcept;
        [[nodiscard]] Result<AgentHistoryStartedTurn> RecordTurn(const AgentHistoryTurnRequest &request,
                                                                 std::optional<AgentHistoryCompletedText> answer);
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Agent
