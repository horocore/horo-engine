#pragma once

/** @file SourceLanguageSession.h @brief Owner-lane session state for revision-fenced language requests. */

#include "Horo/Editor/SourceDocumentService.h"

#include <map>

namespace Horo::Editor {
    /** @brief Negotiation state of the current language connection. */
    enum class SourceLanguageSessionState : std::uint8_t {
        Disconnected,
        Initializing,
        Ready
    };

    /** @brief Language work admitted by the workspace, independent of the transport. */
    enum class SourceLanguageRequestKind : std::uint8_t {
        Diagnostics,
        Completion,
        Hover,
        Definition,
        References,
        Symbols,
        CodeActions
    };

    /** @brief The correlation key scoped to one transport incarnation. Neither field is persisted. */
    struct SourceLanguageRequestId final {
        std::uint64_t session{};
        std::uint64_t request{};
        bool operator==(const SourceLanguageRequestId &) const noexcept = default;
    };

    /** @brief Action needed to synchronize a source snapshot with the current adapter. */
    enum class SourceLanguageSyncKind : std::uint8_t {
        Unchanged,
        Opened,
        Changed,
        Rekeyed
    };

    /** @brief Synchronization outcome. A rekey carries the name the adapter must retire. */
    struct SourceLanguageSync final {
        SourceLanguageSyncKind kind{};
        std::optional<SourceDocumentId> previousPath;
    };

    /** @brief Current document evidence for a reply that passed correlation and revision checks. */
    struct SourceLanguageReply final {
        SourceLanguageRequestKind kind{};
        SourceDocumentSnapshot source;
    };

    /** @brief Owns synchronized documents and finite in-flight work for one workspace language connection.
     * @details Call on the workspace owner lane. Workers may carry immutable snapshots and request IDs.
     * Closing the connection discards its document and request state. Reconnection requires resynchronization.
     */
    class SourceLanguageSession final {
    public:
        SourceLanguageSession() = default;
        SourceLanguageSession(const SourceLanguageSession &) = delete;
        SourceLanguageSession &operator=(const SourceLanguageSession &) = delete;
        SourceLanguageSession(SourceLanguageSession &&) = delete;
        SourceLanguageSession &operator=(SourceLanguageSession &&) = delete;

        static constexpr std::size_t MaximumPendingRequests = 128;

        /** @brief Begins negotiation for a new transport.
         * @return A fresh session stamp or a refusal if a session is already active.
         */
        [[nodiscard]] Result<std::uint64_t> Start();
        /** @brief Completes the current adapter's negotiation.
         * @param session Stamp returned by Start. @return Success only for the negotiating connection.
         */
        [[nodiscard]] Result<void> MarkReady(std::uint64_t session);
        /** @brief Stops the exact connection and retires all of its outstanding work.
         * @param session Connection to stop. @return True when this call stopped the active connection.
         */
        bool Stop(std::uint64_t session) noexcept;

        /** @brief Returns the current transport state. */
        [[nodiscard]] SourceLanguageSessionState State() const noexcept {
            return state_;
        }

        /** @brief Synchronizes the exact current source with the ready connection.
         * @param session Expected connection. @param source Current source lease.
         * @return Sync action or a refusal without replacing the previous lease.
         */
        [[nodiscard]] Result<SourceLanguageSync> Sync(std::uint64_t session, const SourceDocumentSnapshot &source);
        /** @brief Retires a synchronized instance, including its pending requests.
         * @param session Expected connection. @param instance Closed source session.
         * @return True when an admitted document was retired.
         */
        bool Close(std::uint64_t session, DocumentInstanceId instance) noexcept;

        /** @brief Registers a request for an exact, synchronized revision.
         * @param session Expected connection. @param kind Requested feature. @param source Current source lease.
         * @return Correlation ID or a typed refusal. No transport invocation occurs.
         */
        [[nodiscard]] Result<SourceLanguageRequestId> Request(std::uint64_t session, SourceLanguageRequestKind kind,
                                                              const SourceDocumentSnapshot &source);
        /** @brief Retires one request before issuing any best-effort transport cancellation.
         * @param id Exact live request. @return True when the request was retired.
         */
        bool Cancel(SourceLanguageRequestId id) noexcept;
        /** @brief Consumes a reply once and recovers the original request's live document evidence.
         * @param id Correlation key from the reply and its connection.
         * @return Exact current lease or a stale result. Completion never edits a document.
         */
        [[nodiscard]] Result<SourceLanguageReply> Complete(SourceLanguageRequestId id);

    private:
        struct Pending final {
            SourceLanguageRequestKind kind;
            DocumentInstanceId instance;
            std::uint64_t revision;
        };

        /** @brief Confirms a command belongs to the current ready transport. */
        [[nodiscard]] bool IsReady(std::uint64_t session) const noexcept;
        /** @brief Retires all queued work for one document instance. */
        void Retire(DocumentInstanceId instance) noexcept;

        SourceLanguageSessionState state_{SourceLanguageSessionState::Disconnected};
        std::uint64_t session_{};
        std::uint64_t nextRequest_{};
        std::map<DocumentInstanceId, SourceDocumentSnapshot> documents_;
        std::map<std::uint64_t, Pending> pending_;
    };
}  // namespace Horo::Editor
