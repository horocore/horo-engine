#pragma once

/** @file ConsolePresentation.h @brief Runtime terminal input ownership and bounded read-only overlay projections. */
#include "Horo/Debug/CommandExecutor.h"
#include "Horo/Foundation/Logging/StructuredLogStore.h"
#include "Horo/Runtime/Input.h"

namespace Horo::Debug {
    /** @brief Optional terminal controller, independent of editor ImGui or a renderer backend.
     * The graphical host paints its runtime UI from these bounded projections. All methods run on
     * the Input/history owner. Router, executor and configuration outlive this controller; destruction releases focus.
     */
    class ConsoleTerminal final {
    public:
        /** @brief Binds existing owners without creating a UI, input backend or second history store.
         * @param input Existing router. @param executor Same dispatcher used by all console surfaces.
         * @param history Existing visible history. @param enabled Fixed profile/launch admission.
         * @param operations Same operation query authority used by executor; outlives this controller.
         * @param configuration Existing canonical host configuration; captured once per admitted submission, never polled by paint.
         */
        ConsoleTerminal(Input::InputRouter &input, CommandExecutor &executor, ConsoleHistory &history, const IOperationQuery &operations,
                        bool enabled, const ConfigurationService &configuration);
        ConsoleTerminal(const ConsoleTerminal &) = delete;
        ConsoleTerminal &operator=(const ConsoleTerminal &) = delete;
        /** @brief Acquires text focus and a central input exclusion context.
         * @param passThrough Explicit inspection policy; true permits the existing Gameplay context beneath this focused widget.
         * False suppresses lower gameplay contexts. Other GUI/editor, modal, native-dialog and window-focus barriers still apply.
         * @return Success or typed profile/focus/thread failure.
         */
        [[nodiscard]] Result<void> Open(bool passThrough = false);
        /** @brief Releases the RAII context and its text focus. Host calls on suspension/focus loss/shutdown. */
        void Close() noexcept;
        /** @brief Submits through the common policy/parser/dispatcher only while focus is eligible.
         * @param text Command line. @param principal Current authenticated host principal.
         * @return Immediate output or asynchronous store identities; malformed/denied input is never added to history.
         * Worker configuration is the canonical immutable snapshot at this call, independent of later host publications.
         */
        [[nodiscard]] Result<CommandReceipt> Submit(std::string_view text, const DebugCommandContext &principal);
        /** @brief Takes committed Unicode text and native pre-edit from the central focus owner once per input frame.
         * @return Owned delivery or no eligible text; wrong-thread access fails before touching the router.
         * @note Host runtime UI owns editing, selection and native text-input activation; this does not execute a command.
         * Reasserts text ownership only for an already eligible context after modal preemption; stale composition stays suppressed.
         */
        [[nodiscard]] Result<std::optional<Input::TextInputDelivery>> TakeText();
        /** @brief Projects asynchronous results into retained history at most four times per second.
         * @param now Host monotonic time. @return Updated row count or typed thread/source failure.
         * No query occurs without pending rows or when the terminal is disabled. Closing focus does not discard an accepted result.
         */
        [[nodiscard]] Result<std::size_t> RefreshResults(std::chrono::steady_clock::time_point now);
        /** @brief Reports central focus eligibility. @return True only while this context may route. */
        [[nodiscard]] bool IsOpen() const noexcept;
        /** @brief Returns a bounded virtual window without allocating/copying rows.
         * @param first First retained row. @param count At most 128 rows are exposed per window.
         * @return Borrowed history rows, invalidated by history append/clear/destruction.
         */
        [[nodiscard]] std::span<const HistoryEntry> Rows(std::size_t first, std::size_t count) const noexcept;
        /** @brief Returns retained history cardinality for virtual scrolling. @return Bounded row count. */
        [[nodiscard]] std::size_t RowCount() const noexcept;

    private:
        Input::InputRouter &input_;
        CommandExecutor &executor_;
        ConsoleHistory &history_;
        const IOperationQuery &operations_;
        const ConfigurationService &configuration_;
        std::uint64_t operationRevision_{};
        std::chrono::steady_clock::time_point nextResultRefresh_{};
        bool enabled_{};
        std::thread::id owner_;
        Input::InputContextToken focus_;
    };

    /** @brief Explicit category comparison; namespace prefixes stop at a dot boundary. */
    enum class ConsoleLogCategoryMatch : std::uint8_t {
        Exact,
        NamespacePrefix
    };

    /** @brief Display-only log filter, independent of producer severity/category policy. */
    struct ConsoleLogFilter final {
        Log::Level minimumLevel{Log::Level::Trace};
        std::string category; /**< Empty matches every category; maximum 128 UTF-8 bytes. */
        ConsoleLogCategoryMatch categoryMatch{ConsoleLogCategoryMatch::Exact};
        std::string text;       /**< Literal case-sensitive search in the displayed message prefix, maximum 512 UTF-8 bytes. */
        bool collapseRepeats{}; /**< Groups adjacent matching rows only when complete bounded payload/context values match. */
        /** @brief Compares complete display requests. @param other Candidate query. @return Whether every filter field matches. */
        [[nodiscard]] bool operator==(const ConsoleLogFilter &other) const = default;
    };

    /** @brief Bounded display fields borrowing immutable bytes retained by source; copies retain that source lifetime. */
    struct ConsoleLogRow final {
        std::shared_ptr<const Log::StructuredLogRecord> source;
        std::string_view category; /**< At most 128 complete UTF-8 bytes, no per-row text copy. */
        std::string_view message;  /**< At most 4096 complete UTF-8 bytes, also the search domain. */
        std::uint32_t repetitions{1};
        bool truncated{}; /**< The complete record remains in its original log authority. */
    };

    /** @brief Bounded operation text over one immutable snapshot; copied rows retain the original source lifetime. */
    struct ConsoleOperationRow final {
        std::shared_ptr<const OperationStoreSnapshot> snapshot;
        const OperationRecord *source{}; /**< Borrows an element of snapshot; no second operation authority is created. */
        std::string_view title;          /**< At most 512 complete UTF-8 bytes. */
        std::string_view phase;          /**< At most 128 complete UTF-8 bytes. */
        std::string_view message;        /**< At most 4096 complete UTF-8 bytes. */
        std::string_view errorCode;      /**< At most 256 complete UTF-8 bytes; exception/error messages are never projected. */
        bool truncated{};
    };

    /** @brief Read-only adapter over the actual process log and operation stores.
     * No production, persistence, per-sample bus or second history is introduced. Refresh is capped
     * at four times per second. Painting slices borrowed immutable snapshots and allocates no rows.
     * Hidden panels query nothing; snapshots are dropped when hidden. Owner-thread only.
     */
    class ConsoleStoreOverlays final {
    public:
        /** @brief Binds read-only process stores; does not query until shown and refreshed.
         * @param logs Existing log authority. @param operations Existing operation authority.
         */
        ConsoleStoreOverlays(const Log::IStructuredLogQuery &logs, const IOperationQuery &operations);
        /** @brief Changes visibility independently and drops hidden projections.
         * @param logs Logs panel visibility. @param jobs Operations panel visibility.
         */
        void SetVisible(bool logs, bool jobs);
        /** @brief Changes only the display query, with no producer mutation or immediate source access.
         * @param filter Typed bounded candidate; validated before copying. @return Success or typed thread/input failure.
         * The projection changes at the next existing 4 Hz refresh cutoff, even when the source revision is unchanged.
         */
        [[nodiscard]] Result<void> SetLogFilter(const ConsoleLogFilter &filter);
        /** @brief Queries changed snapshots at bounded cadence; disabled path does no allocation or query.
         * @param now Host monotonic presentation time. @return Number of changed projections or a typed thread/source failure.
         * Sources refresh independently; one failure is returned after allowing the other source's bounded refresh.
         */
        [[nodiscard]] Result<std::size_t> Refresh(std::chrono::steady_clock::time_point now);
        /** @brief Returns a virtualized log window, capped to 128 rows and 4096 retained display entries.
         * @param first First row. @param count Requested visible count. @return Borrowed filtered/collapsed display rows.
         * @note The window is invalidated by refresh/hiding; copying a row retains its immutable source and borrowed text lifetime.
         */
        [[nodiscard]] std::span<const ConsoleLogRow> Logs(std::size_t first, std::size_t count) const noexcept;
        /** @brief Returns a virtualized bounded-text operation window under the same display bounds.
         * @param first First row. @param count Requested visible count. @return Borrowed display rows; copies retain the immutable
         * snapshot.
         */
        [[nodiscard]] std::span<const ConsoleOperationRow> Jobs(std::size_t first, std::size_t count) const noexcept;
        /** @brief Returns accepted log cardinality for virtual scrolling. @return Zero while hidden/unavailable. */
        [[nodiscard]] std::size_t LogRowCount() const noexcept;
        /** @brief Returns accepted operation cardinality for virtual scrolling. @return Zero while hidden/unavailable. */
        [[nodiscard]] std::size_t JobRowCount() const noexcept;
        /** @brief Invalidates log row selections/activation when the accepted source/filter/visibility changes. @return Owner revision. */
        [[nodiscard]] std::uint64_t LogProjectionRevision() const noexcept;
        /** @brief Invalidates operation selections/activation when the accepted source/visibility changes. @return Owner revision. */
        [[nodiscard]] std::uint64_t JobProjectionRevision() const noexcept;

    private:
        /** @brief Rebuilds a bounded pointer/view projection only at a refresh cutoff; preserves prior valid rows on source failure. */
        [[nodiscard]] Result<void> RebuildLogRows();
        /** @brief Refreshes only the log source/filter at its own cutoff. @param now Host monotonic time. @return Changed or failure. */
        [[nodiscard]] Result<bool> RefreshLogs(std::chrono::steady_clock::time_point now);
        /** @brief Refreshes only the operation source at its own cutoff. @param now Host monotonic time. @return Changed or failure. */
        [[nodiscard]] Result<bool> RefreshJobs(std::chrono::steady_clock::time_point now);
        /** @brief Validates complete identity/state/progress and bounded display text before atomically publishing a job projection.
         * @param snapshot Owned value from the existing operation authority.
         * @return Success or typed malformed-source failure preserving the last accepted projection and revision.
         */
        [[nodiscard]] Result<void> AcceptJobs(OperationStoreSnapshot snapshot);
        const Log::IStructuredLogQuery &logs_;
        const IOperationQuery &operations_;
        std::thread::id owner_{std::this_thread::get_id()};
        bool logsVisible_{}, jobsVisible_{};
        bool logFilterDirty_{};
        ConsoleLogFilter logFilter_;
        std::vector<ConsoleLogRow> logRows_;
        std::vector<ConsoleLogRow> logScratch_;
        std::chrono::steady_clock::time_point nextLogRefresh_{};
        std::chrono::steady_clock::time_point nextJobRefresh_{};
        std::optional<Log::StructuredLogSnapshot> logSnapshot_;
        std::shared_ptr<const OperationStoreSnapshot> operationSnapshot_;
        std::vector<ConsoleOperationRow> operationRows_;
        std::uint64_t logProjectionRevision_{}, jobProjectionRevision_{};
    };
}  // namespace Horo::Debug
